#include <mod/amlmod.h>
#include <mod/logger.h>
#include <mod/config.h>
#include <mod/menu-api.h>

#include <cstdint>

MYMODCFG(net.psdk.samod.menukit, SA Menu Kit, 0.1, Jean7z)

/* The engine lives in mod/menu.cpp. It resolves everything by symbol; we only
   hand it the IAML* instance that MYMOD already created for this TU. */
extern "C" bool MenuKit_Load(IAML* aml);

ON_MOD_LOAD()
{
    logger->SetTag("MenuKit");
    if(MenuKit_Load(aml))
        logger->Info("SA Menu Kit v0.1 loaded - GetMenuAPI() exported");
    else
        logger->Error("SA Menu Kit failed to initialise");
}