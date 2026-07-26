#include "TicoChainload.h"

#include "TicoConfig.h"

#include <cstdio>
#include <string>
#include <sys/stat.h>

#include <switch.h>
#include <switch/runtime/env.h>

namespace Tico
{

void ChainloadLauncher(const LogCallback& log)
{
  struct stat st = {};
  if (stat(Paths::LauncherNro, &st) == 0)
  {
    char args[512];
    std::snprintf(args, sizeof(args), "%s --resume", Paths::LauncherNro);
    envSetNextLoad(Paths::LauncherNro, args);
    if (log)
      log(std::string("Chainloading back to ") + Paths::LauncherNro);
  }
  else if (log)
  {
    log(std::string("No tico launcher found at ") + Paths::LauncherNro);
  }
}

} // namespace Tico
