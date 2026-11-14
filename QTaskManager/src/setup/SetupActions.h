#pragma once
#include "monitoring/SetupTypes.h"
namespace monitoring
{
class SetupPlatform
{
  public:
    virtual ~SetupPlatform() = default;
    virtual uint32_t validateManagedService() = 0;
    virtual uint32_t install(bool update) = 0;
    virtual uint32_t enable() = 0;
    virtual uint32_t disable() = 0;
    virtual uint32_t remove() = 0;
};
SetupResult runSetup(const SetupRequest &, SetupPlatform &);
SetupResult runSetup(const SetupRequest &);
} // namespace monitoring
