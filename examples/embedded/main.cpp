#include <mod/amlmod.h>
#include <mod/logger.h>
#include <mod/menu-api.h>

MYMOD(net.psdk.samod.menukit.embedded, MenuKit Embedded Example, 1.0, Jean7z)

static const MenuKitAPI* g_menuKit;

static void DrawFrame(void*)
{
    g_menuKit->DrawRect(60, 60, 300, 100, 0x101820C0u, 1);
    g_menuKit->DrawText(72, 72, "EMBEDDED", 2, 0x38BDF8FFu);
}

ON_MOD_LOAD()
{
    g_menuKit = MenuKit_InitEmbedded(aml);
    if(!g_menuKit)
    {
        logger->Error("MenuKit embedded initialization failed");
        return;
    }

    void* button = g_menuKit->AddButton(0, "_shoot", 320, 380, 20, NULL, NULL, NULL);
    if(button)
    {
        g_menuKit->SetText(button, "MENU");
        g_menuKit->SetSize(button, 220, 90);
    }
    g_menuKit->SetTick(DrawFrame, NULL);
}

ON_MOD_UNLOAD()
{
    MenuKit_Shutdown();
}
