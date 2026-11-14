#include "PackageSecurity.h"
namespace monitoring
{
bool isProtectedInstallAcl(PSECURITY_DESCRIPTOR sd)
{
    if (!sd || !IsValidSecurityDescriptor(sd))
        return false;
    SECURITY_DESCRIPTOR_CONTROL control{};
    DWORD revision{};
    if (!GetSecurityDescriptorControl(sd, &control, &revision) || !(control & SE_DACL_PROTECTED))
        return false;
    PSID owner = nullptr;
    BOOL defaulted{};
    auto trusted = [](PSID sid) {
        return sid &&
               (IsWellKnownSid(sid, WinLocalSystemSid) || IsWellKnownSid(sid, WinBuiltinAdministratorsSid));
    };
    if (!GetSecurityDescriptorOwner(sd, &owner, &defaulted) || !trusted(owner))
        return false;
    PACL acl = nullptr;
    BOOL present{};
    if (!GetSecurityDescriptorDacl(sd, &present, &acl, &defaulted) || !present || !acl)
        return false;
    constexpr DWORD mutation = FILE_WRITE_DATA | FILE_APPEND_DATA | FILE_WRITE_EA | FILE_WRITE_ATTRIBUTES |
                               DELETE | WRITE_DAC | WRITE_OWNER | FILE_DELETE_CHILD | GENERIC_WRITE |
                               GENERIC_ALL;
    for (DWORD i = 0; i < acl->AceCount; ++i)
    {
        void *raw = nullptr;
        if (!GetAce(acl, i, &raw))
            return false;
        auto *h = static_cast<ACE_HEADER *>(raw);
        if (h->AceType == ACCESS_ALLOWED_ACE_TYPE)
        {
            auto *ace = static_cast<ACCESS_ALLOWED_ACE *>(raw);
            if ((ace->Mask & mutation) && !trusted(&ace->SidStart))
                return false;
        }
        else if (h->AceType != ACCESS_DENIED_ACE_TYPE)
            return false;
    }
    return true;
}
} // namespace monitoring
