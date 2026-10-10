#include <mod/amlmod.h>
#include <mod/logger.h>
#include <mod/config.h>
#include <mod/menu-api.h>

#include <cstdint>

MYMODCFG(net.psdk.samod.menukit, SA Menu Kit, 1.2, Jean7z)

/* The engine implementation resolves the game symbols through AML. */
extern "C" bool  MenuKit_Load(IAML* aml);
extern "C" void MenuKit_Shutdown(void);

ON_MOD_LOAD()
{
    logger->SetTag("MenuKit");
    if(MenuKit_Load(aml))
        logger->Info("SA Menu Kit API v%d loaded - GetMenuAPI() exported", MENUKIT_API_VERSION);
    else
        logger->Error("SA Menu Kit failed to initialise");
}

/* Release widgets owned by this framework when AML unloads the mod. */
ON_MOD_UNLOAD()
{
    MenuKit_Shutdown();
    logger->Info("SA Menu Kit unloaded - widgets freed");
}