#include "DriverFsr4.h"

#include <FidelityFX/upscalers/include/ffx_upscale.h>

#include <intrin.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <format>
#include <mutex>
#include <span>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace DriverFsr4
{
	namespace
	{
		// ---------------------------------------------------------------------------------------
		// The driver library's provider interface (UpdateFfxApiProvider), as OptiScaler documents it
		// ---------------------------------------------------------------------------------------

		struct ProviderInterface
		{
			uint64_t versionId;
			const char* versionName;
			bool (*canProvide)(uint64_t descType);
			ffxReturnCode_t (*createContext)(ffxContext* context, ffxCreateContextDescHeader* desc, const ffxAllocationCallbacks* memCb);
			ffxReturnCode_t (*destroyContext)(ffxContext* context, const ffxAllocationCallbacks* memCb);
			ffxReturnCode_t (*configure)(ffxContext* context, const ffxConfigureDescHeader* desc);
			ffxReturnCode_t (*query)(ffxContext* context, ffxQueryDescHeader* desc);
			ffxReturnCode_t (*dispatch)(ffxContext* context, const ffxDispatchDescHeader* desc);
		};

		struct ExternalProviderData
		{
			uint32_t structVersion;
			uint64_t descType;
			ProviderInterface provider;
			bool (*isSupported)(const void* backend);  // struct version 3
			void* reserved;

			// The library wants all 0x60 bytes for version 3 and 0x50 for older versions
			static constexpr uint32_t SizeFor(uint32_t a_structVersion) { return a_structVersion >= 3 ? sizeof(ExternalProviderData) : 0x50; }
		};
		static_assert(offsetof(ExternalProviderData, isSupported) == 0x50);
		static_assert(sizeof(ExternalProviderData) == 0x60);

		using PFN_UpdateFfxApiProvider = HRESULT(STDMETHODCALLTYPE*)(void* a_data, uint32_t a_dataSize);

		// ---------------------------------------------------------------------------------------
		// State
		// ---------------------------------------------------------------------------------------

		HMODULE g_module = nullptr;
		PFN_UpdateFfxApiProvider g_updateProvider = nullptr;
		ProviderInterface g_provider{};
		std::string g_versionName;
		std::string g_failure;
		bool g_available = false;
		Model g_model = Model::Int8;

		decltype(&GetModuleHandleW) g_originalGetModuleHandleW = nullptr;
		std::atomic<bool> g_priming = false;

		ffxFunctions g_original{};
		std::unordered_set<ffxContext> g_contexts;
		std::mutex g_contextsMutex;

		// ---------------------------------------------------------------------------------------
		// Module lookups made by the driver library
		// ---------------------------------------------------------------------------------------

		HMODULE CallerModule(void* a_returnAddress)
		{
			HMODULE module = nullptr;
			GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
				static_cast<LPCWSTR>(a_returnAddress), &module);
			return module;
		}

		// Only the driver library calls this, through its patched import. While it sets its SDK
		// version it gets itself for every AMD SDK library it looks up: it keeps the version of the
		// first one it checks for the whole process, and its own file version is the newest SDK.
		// Afterwards it gets itself for the legacy amd_fidelityfx_dx12.dll that struct version 1
		// requests check, which the game never loads.
		HMODULE WINAPI HookGetModuleHandleW(LPCWSTR a_name)
		{
			const HMODULE caller = CallerModule(_ReturnAddress());
			if (caller == g_module && a_name) {
				if (g_priming.load() && _wcsnicmp(a_name, L"amd_fidelityfx_", 15) == 0)
					return g_module;
			}
			HMODULE module = g_originalGetModuleHandleW ? g_originalGetModuleHandleW(a_name) : GetModuleHandleW(a_name);
			if (!module && caller == g_module && a_name && _wcsicmp(a_name, L"amd_fidelityfx_dx12.dll") == 0)
				module = g_module;
			return module;
		}

		bool PatchImport(HMODULE a_module, const char* a_dll, const char* a_function, void* a_hook, void** a_original)
		{
			auto* base = reinterpret_cast<uint8_t*>(a_module);
			auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
			auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
			const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
			if (!dir.VirtualAddress)
				return false;

			for (auto* desc = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress); desc->Name; ++desc) {
				if (_stricmp(reinterpret_cast<const char*>(base + desc->Name), a_dll) != 0)
					continue;
				auto* names = reinterpret_cast<IMAGE_THUNK_DATA64*>(base + (desc->OriginalFirstThunk ? desc->OriginalFirstThunk : desc->FirstThunk));
				auto* slots = reinterpret_cast<IMAGE_THUNK_DATA64*>(base + desc->FirstThunk);
				for (; names->u1.AddressOfData; ++names, ++slots) {
					if (IMAGE_SNAP_BY_ORDINAL64(names->u1.Ordinal))
						continue;
					auto* byName = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
					if (std::strcmp(reinterpret_cast<const char*>(byName->Name), a_function) != 0)
						continue;
					DWORD protection = 0;
					if (!VirtualProtect(&slots->u1.Function, sizeof(void*), PAGE_READWRITE, &protection))
						return false;
					*a_original = reinterpret_cast<void*>(slots->u1.Function);
					slots->u1.Function = reinterpret_cast<ULONGLONG>(a_hook);
					VirtualProtect(&slots->u1.Function, sizeof(void*), protection, &protection);
					return true;
				}
			}
			return false;
		}

		// ---------------------------------------------------------------------------------------
		// Model selection: each FSR 4 model is its own provider class that only differs in its
		// device support check, and the first class that reports support runs. Answering that
		// check (vtable slot 2 of the SDK 2.x provider classes) picks the model.
		// ---------------------------------------------------------------------------------------

		struct TypeDescriptor
		{
			void* vtable;
			void* spare;
			char name[1];
		};

		struct CompleteObjectLocator
		{
			uint32_t signature;
			uint32_t offset;
			uint32_t constructorDisplacementOffset;
			uint32_t typeDescriptor;
			uint32_t classHierarchyDescriptor;
			uint32_t self;
		};

		enum class SectionKind
		{
			Code,
			ReadOnlyData,
			WritableData,
		};

		std::vector<std::span<const uint8_t>> Sections(HMODULE a_module, SectionKind a_kind)
		{
			std::vector<std::span<const uint8_t>> sections;
			auto* base = reinterpret_cast<const uint8_t*>(a_module);
			auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
			auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
			auto* section = IMAGE_FIRST_SECTION(nt);
			for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section) {
				const bool code = section->Characteristics & IMAGE_SCN_MEM_EXECUTE;
				const bool writable = section->Characteristics & IMAGE_SCN_MEM_WRITE;
				const auto kind = code ? SectionKind::Code : (writable ? SectionKind::WritableData : SectionKind::ReadOnlyData);
				if (kind == a_kind)
					sections.emplace_back(base + section->VirtualAddress, section->Misc.VirtualSize);
			}
			return sections;
		}

		void** FindVtable(HMODULE a_module, std::string_view a_className)
		{
			auto* base = reinterpret_cast<const uint8_t*>(a_module);
			const TypeDescriptor* typeDescriptor = nullptr;
			for (auto prefix : { ".?AV", ".?AU" }) {
				const auto mangled = std::format("{}{}@@", prefix, a_className);
				const std::string_view needle(mangled.c_str(), mangled.size() + 1);  // the terminator keeps Foo from matching Foo_Bar
				for (auto section : Sections(a_module, SectionKind::WritableData)) {
					const std::string_view data(reinterpret_cast<const char*>(section.data()), section.size());
					if (const auto position = data.find(needle); position != std::string_view::npos) {
						typeDescriptor = reinterpret_cast<const TypeDescriptor*>(data.data() + position - offsetof(TypeDescriptor, name));
						break;
					}
				}
				if (typeDescriptor)
					break;
			}
			if (!typeDescriptor)
				return nullptr;

			const auto typeDescriptorRva = static_cast<uint32_t>(reinterpret_cast<const uint8_t*>(typeDescriptor) - base);
			const CompleteObjectLocator* locator = nullptr;
			for (auto section : Sections(a_module, SectionKind::ReadOnlyData)) {
				for (size_t offset = 0; offset + sizeof(CompleteObjectLocator) <= section.size(); offset += alignof(uint32_t)) {
					auto* candidate = reinterpret_cast<const CompleteObjectLocator*>(section.data() + offset);
					const auto candidateRva = static_cast<uint32_t>(section.data() + offset - base);
					if (candidate->signature == 1 && candidate->offset == 0 && candidate->typeDescriptor == typeDescriptorRva && candidate->self == candidateRva) {
						locator = candidate;
						break;
					}
				}
				if (locator)
					break;
			}
			if (!locator)
				return nullptr;

			// The entry right before the first virtual function points to the locator
			for (auto section : Sections(a_module, SectionKind::ReadOnlyData)) {
				auto* entries = reinterpret_cast<const uintptr_t*>(section.data());
				const size_t count = section.size() / sizeof(uintptr_t);
				for (size_t i = 0; i + 1 < count; ++i) {
					if (entries[i] == reinterpret_cast<uintptr_t>(locator))
						return reinterpret_cast<void**>(const_cast<uintptr_t*>(&entries[i + 1]));
				}
			}
			return nullptr;
		}

		enum class ProviderModel
		{
			Fp8,
			Int8,
			Int8Navi2,
			Int8Lite,
		};

		struct ModelProvider
		{
			ProviderModel model;
			const char* name;
			const char* className;
		};

		// In the order FFX checks them
		constexpr std::array<ModelProvider, 4> kModelProviders{ {
			{ ProviderModel::Fp8, "FP8", "ffxProvider_FSR4" },
			{ ProviderModel::Int8, "INT8", "ffxProvider_FSR4_Int8" },
			{ ProviderModel::Int8Navi2, "INT8 RDNA2", "ffxProvider_FSR4_Int8_Navi2" },
			{ ProviderModel::Int8Lite, "INT8 Lite", "ffxProvider_FSR4Lite_Int8" },
		} };

		constexpr size_t kIsSupportedIndex = 2;
		using PFN_IsSupported = bool (*)(void* a_provider, void* a_device);

		struct PatchedProvider
		{
			void** vtable;
			const ModelProvider* provider;
			PFN_IsSupported original;
		};
		std::vector<PatchedProvider> g_patchedProviders;
		std::atomic<const char*> g_activeModel = nullptr;

		ProviderModel SelectedModel()
		{
			return g_model == Model::Int8Lite ? ProviderModel::Int8Lite : ProviderModel::Int8;
		}

		bool HookIsSupported(void* a_provider, void* a_device)
		{
			auto** vtable = *static_cast<void***>(a_provider);
			for (const auto& patched : g_patchedProviders) {
				if (patched.vtable != vtable)
					continue;
				// The library's own check limits INT8 to RDNA 3 and Lite to AMD APUs; every GPU with
				// DP4a runs the INT8 networks, so the selected one reports support and the rest don't.
				const bool supported = patched.provider->model == SelectedModel();
				if (supported && a_device)
					g_activeModel = patched.provider->name;
				return supported;
			}
			return false;
		}

		bool HookModels(HMODULE a_module)
		{
			std::vector<PatchedProvider> found;
			for (const auto& provider : kModelProviders) {
				if (auto** vtable = FindVtable(a_module, provider.className))
					found.push_back({ vtable, &provider, nullptr });
			}
			const bool hasSelected = std::ranges::any_of(found, [](const PatchedProvider& a_entry) { return a_entry.provider->model == SelectedModel(); });
			if (found.size() < 2 || !hasSelected) {
				g_failure = std::format("the library has {} FSR 4 model classes and not the {} model", found.size(), ModelName(g_model));
				return false;
			}
			for (auto& entry : found) {
				void** slot = &entry.vtable[kIsSupportedIndex];
				DWORD protection = 0;
				if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &protection))
					continue;
				entry.original = reinterpret_cast<PFN_IsSupported>(*slot);
				*slot = reinterpret_cast<void*>(&HookIsSupported);
				VirtualProtect(slot, sizeof(void*), protection, &protection);
				g_patchedProviders.push_back(entry);
				logger::info("[FidelityFX] Driver FSR 4: hooked the support check of {}", entry.provider->className);
			}
			return !g_patchedProviders.empty();
		}

		// ---------------------------------------------------------------------------------------
		// FFX function table shims
		// ---------------------------------------------------------------------------------------

		bool Owns(ffxContext* a_context)
		{
			if (!a_context || !*a_context)
				return false;
			std::scoped_lock lock(g_contextsMutex);
			return g_contexts.contains(*a_context);
		}

		// The override that selects the driver provider, unlinked from the chain while the provider
		// creates its context (it is a loader descriptor the provider does not know).
		ffxReturnCode_t ShimCreateContext(ffxContext* a_context, ffxCreateContextDescHeader* a_desc, const ffxAllocationCallbacks* a_memCb)
		{
			if (g_available && a_desc && a_desc->type == FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE) {
				ffxApiHeader* previous = a_desc;
				for (ffxApiHeader* link = a_desc->pNext; link; previous = link, link = link->pNext) {
					if (link->type != FFX_API_DESC_TYPE_OVERRIDE_VERSION || reinterpret_cast<ffxOverrideVersion*>(link)->versionId != g_provider.versionId)
						continue;
					previous->pNext = link->pNext;
					const auto result = g_provider.createContext(a_context, a_desc, a_memCb);
					previous->pNext = link;
					logger::info("[FidelityFX] Driver FSR 4 context created: {}", static_cast<uint32_t>(result));
					if (result == FFX_API_RETURN_OK && a_context && *a_context) {
						std::scoped_lock lock(g_contextsMutex);
						g_contexts.insert(*a_context);
					}
					return result;
				}
			}
			return g_original.CreateContext(a_context, a_desc, a_memCb);
		}

		ffxReturnCode_t ShimDestroyContext(ffxContext* a_context, const ffxAllocationCallbacks* a_memCb)
		{
			if (!Owns(a_context))
				return g_original.DestroyContext(a_context, a_memCb);
			const ffxContext handle = *a_context;
			const auto result = g_provider.destroyContext(a_context, a_memCb);
			std::scoped_lock lock(g_contextsMutex);
			g_contexts.erase(handle);
			return result;
		}

		ffxReturnCode_t ShimConfigure(ffxContext* a_context, const ffxConfigureDescHeader* a_desc)
		{
			return Owns(a_context) ? g_provider.configure(a_context, a_desc) : g_original.Configure(a_context, a_desc);
		}

		ffxReturnCode_t ShimDispatch(ffxContext* a_context, const ffxDispatchDescHeader* a_desc)
		{
			return Owns(a_context) ? g_provider.dispatch(a_context, a_desc) : g_original.Dispatch(a_context, a_desc);
		}

		// Upscale version lists get the driver provider first, so a version override can pick it.
		ffxReturnCode_t ShimQuery(ffxContext* a_context, ffxQueryDescHeader* a_desc)
		{
			if (Owns(a_context))
				return g_provider.query(a_context, a_desc);
			if (!g_available || a_context || !a_desc || a_desc->type != FFX_API_QUERY_DESC_TYPE_GET_VERSIONS)
				return g_original.Query(a_context, a_desc);

			auto* versions = reinterpret_cast<ffxQueryDescGetVersions*>(a_desc);
			if (versions->createDescType != FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE || !versions->outputCount)
				return g_original.Query(a_context, a_desc);

			if (!versions->versionIds && !versions->versionNames) {
				const auto result = g_original.Query(a_context, a_desc);
				if (result == FFX_API_RETURN_OK)
					*versions->outputCount += 1;
				return result;
			}

			const uint64_t capacity = *versions->outputCount;
			if (capacity == 0)
				return g_original.Query(a_context, a_desc);
			ffxQueryDescGetVersions inner = *versions;
			uint64_t innerCount = capacity - 1;
			inner.outputCount = &innerCount;
			inner.versionIds = versions->versionIds ? versions->versionIds + 1 : nullptr;
			inner.versionNames = versions->versionNames ? versions->versionNames + 1 : nullptr;
			ffxReturnCode_t result = FFX_API_RETURN_OK;
			if (innerCount)
				result = g_original.Query(a_context, &inner.header);
			else
				innerCount = 0;
			if (result != FFX_API_RETURN_OK)
				return result;
			if (versions->versionIds)
				versions->versionIds[0] = g_provider.versionId;
			if (versions->versionNames)
				versions->versionNames[0] = g_versionName.c_str();
			*versions->outputCount = innerCount + 1;
			return FFX_API_RETURN_OK;
		}
	}

	const char* ModelName(Model a_model)
	{
		return a_model == Model::Int8Lite ? "INT8 Lite" : "INT8";
	}

	bool Load(const std::filesystem::path& a_dllPath, Model a_model)
	{
		if (g_module)
			return g_available;
		g_model = a_model;

		std::error_code ec;
		if (!std::filesystem::exists(a_dllPath, ec)) {
			g_failure = "amdxcffx64.dll is not in the FidelityFX folder";
			return false;
		}
		g_module = LoadLibraryExW(a_dllPath.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
		if (!g_module) {
			g_failure = std::format("amdxcffx64.dll did not load (error {})", GetLastError());
			return false;
		}
		g_updateProvider = reinterpret_cast<PFN_UpdateFfxApiProvider>(GetProcAddress(g_module, "UpdateFfxApiProvider"));
		if (!g_updateProvider) {
			g_failure = "amdxcffx64.dll has no UpdateFfxApiProvider";
			return false;
		}

		if (PatchImport(g_module, "KERNEL32.dll", "GetModuleHandleW", reinterpret_cast<void*>(&HookGetModuleHandleW), reinterpret_cast<void**>(&g_originalGetModuleHandleW))) {
			ExternalProviderData priming{};
			priming.structVersion = 2;
			priming.descType = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
			g_priming = true;
			const HRESULT result = g_updateProvider(&priming, ExternalProviderData::SizeFor(2));
			g_priming = false;
			logger::info("[FidelityFX] Driver FSR 4: SDK version set from the library itself ({:#x})", static_cast<uint32_t>(result));
		} else {
			logger::warn("[FidelityFX] Driver FSR 4: could not patch the library's GetModuleHandleW import");
		}

		if (!HookModels(g_module))
			return false;

		// SDK 2.x sends struct version 2; the first FSR 4 drivers only fill in version 1.
		for (uint32_t structVersion : { 2u, 1u }) {
			ExternalProviderData data{};
			data.structVersion = structVersion;
			data.descType = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
			const HRESULT result = g_updateProvider(&data, ExternalProviderData::SizeFor(structVersion));
			if (SUCCEEDED(result) && data.provider.createContext) {
				g_provider = data.provider;
				g_versionName = data.provider.versionName ? data.provider.versionName : "FSR 4 (driver library)";
				g_provider.versionName = nullptr;
				g_available = true;
				g_failure.clear();
				logger::info("[FidelityFX] Driver FSR 4: provider '{}' (id {:#x}, struct version {}), model {}", g_versionName, g_provider.versionId, structVersion, ModelName(g_model));
				return true;
			}
			logger::info("[FidelityFX] Driver FSR 4: UpdateFfxApiProvider struct version {} returned {:#x}", structVersion, static_cast<uint32_t>(result));
		}
		g_failure = "the library did not hand out an upscale provider";
		return false;
	}

	void InstallShims(ffxFunctions& a_module)
	{
		if (!g_available || !a_module.CreateContext || g_original.CreateContext)
			return;
		g_original = a_module;
		a_module.CreateContext = &ShimCreateContext;
		a_module.DestroyContext = &ShimDestroyContext;
		a_module.Configure = &ShimConfigure;
		a_module.Query = &ShimQuery;
		a_module.Dispatch = &ShimDispatch;
	}

	bool IsAvailable() { return g_available; }
	uint64_t VersionId() { return g_available ? g_provider.versionId : 0; }
	const std::string& VersionName() { return g_versionName; }
	const char* ActiveModelName() { return g_activeModel.load(); }
	const std::string& FailureReason() { return g_failure; }
}
