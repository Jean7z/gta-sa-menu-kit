#include <mod/amlmod.h>
#include <mod/logger.h>
#include <mod/config.h>
#include <mod/menu-api.h>

#include <cstdint>

MYMODCFG(net.psdk.samod.menukit, SA Menu Kit, 0.1, Jean7z)

/* The engine lives in mod/menu.cpp. It resolves everything by symbol; we only
   hand it the IAML* instance that MYMOD already created for this TU. */
extern "C" bool  MenuKit_Load(IAML* aml);
extern "C" void MenuKit_Shutdown(void);

ON_MOD_LOAD()
{
    logger->SetTag("MenuKit");
    if(MenuKit_Load(aml))
        logger->Info("SA Menu Kit v0.1 loaded - GetMenuAPI() exported");
    else
        logger->Error("SA Menu Kit failed to initialise");
}

/* Real dispose on unload: every widget this framework still owns is destroyed
   through its virtual destructor and the pool slots are handed back to the
   game, so a reload does not leak one 0x200 object per widget. "Not
   guaranteed" per amlmod.h, so if the host never fires it the process death
   reclaims everything anyway. */
ON_MOD_UNLOAD()
{
    MenuKit_Shutdown();
    logger->Info("SA Menu Kit unloaded - widgets freed");
}