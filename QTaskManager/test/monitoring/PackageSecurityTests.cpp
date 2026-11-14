#include "TestSupport.h"
#include "setup/PackageSecurity.h"
#include <sddl.h>
using namespace monitoring;
bool check(const wchar_t *text)
{
    PSECURITY_DESCRIPTOR sd = nullptr;
    CHECK(ConvertStringSecurityDescriptorToSecurityDescriptorW(text, SDDL_REVISION_1, &sd, nullptr));
    bool safe = isProtectedInstallAcl(sd);
    LocalFree(sd);
    return safe;
}
int main()
{
    CHECK(check(L"O:BAG:BAD:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;FRFX;;;BU)"));
    CHECK(!check(L"O:BAG:BAD:P(A;;FA;;;BU)"));
    CHECK(!check(L"O:BAG:BAD:P(A;;GW;;;IU)"));
    CHECK(!check(L"O:BAG:BAD:(A;;FA;;;BA)"));
    CHECK(!isProtectedInstallAcl(nullptr));
}
