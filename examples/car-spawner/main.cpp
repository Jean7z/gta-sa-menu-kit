/* Car Spawner - client demo of the SA Menu Kit API v2.
   ROOT group (menu 0): one button. Tapping it opens MENU 1 via OpenMenu,
   which replaces the root with FOUR stacked buttons, each spawning a
   different vehicle and closing the menu.
   Pure consumer: all menu machinery lives in MenuKit.
   Requires the SA Menu Kit framework (AML_PSDK_MenuKit64) loaded first. */
#include <mod/amlmod.h>
#include <mod/logger.h>
#include <mod/config.h>
#include <mod/menu-api.h>

#include <cstdint>

MYMODCFG(net.psdk.samod.carspawner, Car Spawner, 0.3, Jean7z)

/* CCheat::VehicleCheat - the game's own vehicle spawn (used by the phone
   cheats): requests the model via CStreaming and creates the CVehicle in
   front of the player. Resolved by name, no hardcoded offsets. */
static void (*pVehicleCheat)(int modelId) = NULL;

static const MenuKitAPI* s_api = NULL;

static const int MENU_CARS = 1;

/* The four selectable cars. All buttons share the SAME texture ("shoot"),
   so the player picks by POSITION: top = Infernus, then Banshee, Cheetah,
   bottom = Sanchez (the pointer to the entry is the AddButton userdata). */
struct CarOption { int model; const char* name; };
static const CarOption kCars[] = {
    { 411, "Infernus" },
    { 429, "Banshee" },
    { 415, "Cheetah" },
    { 468, "Sanchez" },
};

static void OnRootButton(void* userdata)
{
    logger->Info("CarSpawner: opening car menu (menu %d)", MENU_CARS);
    if(!s_api || s_api->OpenMenu(MENU_CARS) != 0)
        logger->Error("CarSpawner: could not open menu %d", MENU_CARS);
}

static void OnSpawnCar(void* userdata)
{
    const CarOption* car = (const CarOption*)userdata;
    logger->Info("CarSpawner: spawning %s (%d)", car->name, car->model);
    pVehicleCheat(car->model);
    if(s_api) s_api->CloseMenu(); /* spawn and get back to the root */
}

/* v3 custom-icon test: this button paints icon-test.png (deployed next to the
   .so) instead of the game's "shoot" texture. */
static void OnIconTest(void* userdata)
{
    logger->Info("CarSpawner: custom-icon button pressed (feature OK)");
}

ON_MOD_LOAD()
{
    logger->SetTag("CarSpawner");

    uintptr_t pGameHandle = aml->GetLib("libGTASA.so");
    pVehicleCheat = (void(*)(int))aml->GetSym(pGameHandle, "_ZN6CCheat12VehicleCheatEi");
    if(!pVehicleCheat)
    {
        logger->Error("CarSpawner: CCheat::VehicleCheat symbol not found");
        return;
    }

    s_api = MenuKit_GetAPI(aml);
    if(!s_api)
    {
        logger->Error("CarSpawner: MenuKit framework not loaded (load AML_PSDK_MenuKit64 first)");
        return;
    }
    if(s_api->version < 2)
    {
        logger->Error("CarSpawner: MenuKit API v2 required (got v%u)", s_api->version);
        s_api = NULL;
        return;
    }

    /* "shoot" is a REAL texture name in the game's image DB (the native attack
       button CWidgetButtonAttackC2 references it at libGTASA.so+0x83d611).
       The ctor resolves the texture by name; a bogus name leaves the sprite
       empty -> invisible button. NOTE: no leading underscore - "_shoot" does
       NOT exist and produces a NULL sprite (verified in a live pool dump).

       Coordinates live in the game's VIRTUAL 640x448 screen space (not 0-1
       normalized): CWidget::Update computes the on-screen rect as
       left/right = (OriginX +/- ScaleX) * screenW/640 and
       top/bottom  = (OriginY +/- ScaleY) * screenH/448.
       Native attack button sits at Origin(560,380) Scale(50,30).
       -> root below it, center-ish: Origin(320,224) Scale(60,60). */

    /* ROOT group (menu 0): visible while no menu is open. */
    void* root = s_api->AddButton(0, "shoot", 320.0f, 224.0f, 60.0f, OnRootButton, NULL, NULL);
    if(!root) logger->Error("CarSpawner: could not add root widget");
    else logger->Info("CarSpawner: root button added (tap to open car menu)");

    /* v3: custom-icon button, below the root one. PNG lives in the mods folder,
       loaded once and cached by the engine (replicate the game's own texture
       pipeline). 128x128 source, same ballpark as the native UI icons so the
       scale is close to 1:1 on this button's rect. */
    void* itest = s_api->AddButton(0, "shoot", 320.0f, 400.0f, 45.0f, OnIconTest, NULL,
                                   "/storage/emulated/0/Android/data/com.rockstargames.gtasa/mods/icon-test.png");
    if(!itest) logger->Error("CarSpawner: could not add icon-test widget");
    else logger->Info("CarSpawner: icon-test button added (custom PNG icon)");

    /* MENU 1: four spawn buttons stacked vertically in the 640x448 space,
       centered on x=320. Scale 40 -> 80px tall, stepped by 95px -> clear
       gaps, whole list from y=55..405 fits the screen and stays clear of
       the native attack button at (560,380). Same texture for all - the
       pick is positional (top=Infernus ... bottom=Sanchez). The framework
       verifies N-widget groups (2 widgets built on open, earlier demo). */
    for(int i = 0; i < 4; ++i)
    {
        float y = 55.0f + (float)i * 95.0f;
        void* h = s_api->AddButton(MENU_CARS, "shoot", 320.0f, y, 40.0f,
                                   OnSpawnCar, (void*)&kCars[i], NULL);
        if(!h) logger->Error("CarSpawner: could not add %s (%d) button",
                             kCars[i].name, kCars[i].model);
        else logger->Info("CarSpawner: %s (%d) button added at y=%g",
                          kCars[i].name, kCars[i].model, y);
    }
}