/* Car Selector v2 - client demo of the SA Menu Kit API v4.
   ROOT group (menu 0): one button labeled "CARS". Tapping it opens MENU 1
   with THREE buttons: a left arrow "<", an INVISIBLE center select button,
   and a right arrow ">".
     - Left/right arrows: tap SPAWNS the highlighted car immediately. The
       previously spawned car is despawned first, so at most one spawned car
       exists ("el anterior desaparece, solo queda el seleccionado").
     - Center button: no label, invisible (bogus texture -> NULL sprite),
       keeps the v1 select function: spawn the current car + close the menu.
   Spawn = MANUAL construction replicating CCheat::VehicleCheat's spawn half
   (the cheat itself is NOT used: it aborts silently on 3 internal guards,
   which is why v1's cheat call often spawned nothing, and the pool-scan
   tracking in v2 lost the pointer whenever the cheat DID work, so
   despawn never fired and cars accumulated). We replicate the cheat's
   post-guard code with exported symbols: the same streaming calls
   (RequestModel + LoadAllRequestedModels), CVehicle::operator new with the
   exact class size (Automobile 0xbc8 / Bike 0xa18), the imported ctor,
   the same post-ctor flag writes, the car placed 4.0 units in front of the
   player facing his heading + 90 deg, then CWorld::Add +
   ClearSpaceForMissionEntity + PlaceOnRoadProperly. The ctor returns the
    exact CVehicle* -> direct pointer tracking, no pool scan. The ctor's
    random paint job (CCarCols) is overwritten with a fixed colour PER CAR
    (colour slots veh+0x574/0x575, palette indices into the game's own
    ms_vehicleColourTable) so each model keeps its own stable colour on
    every re-spawn and cars don't "change colour" when re-spawned.
    Despawn = CWorld::Remove + the class deleting destructor (the game's own
   remove-car path). Safe guard: never despawn the car the player is driving.
   Pure consumer: all menu machinery lives in MenuKit.
   Requires the SA Menu Kit framework (AML_PSDK_MenuKit64) loaded first. */
#include <mod/amlmod.h>
#include <mod/logger.h>
#include <mod/config.h>
#include <mod/menu-api.h>

#include <cstdint>

MYMODCFG(net.psdk.samod.carspawner, Car Selector, 0.5, Jean7z)

/* Game symbols, all resolved by name (verified against libGTASA arm64
   exports). No hardcoded offsets except the class member offsets copied
   verbatim from CCheat::VehicleCheat. */
static void (*pRequestModel)(int model, int flags) = NULL;       /* CStreaming::RequestModel(model, flags) */
static void (*pLoadAllRequestedModels)(bool) = NULL;             /* CStreaming::LoadAllRequestedModels */
static void* (*pVehicleNew)(size_t sz) = NULL;                   /* CVehicle::operator new */
static void (*pAutoCtor)(void*, int, uint8_t, uint8_t) = NULL;   /* CAutomobile::CAutomobile(model, createdBy, addToWorld) */
static void (*pBikeCtor)(void*, int, uint8_t) = NULL;            /* CBike::CBike(model, createdBy) */
static void (*pWorldAdd)(void* entity) = NULL;                   /* CWorld::Add(CEntity*) */
static void (*pClearSpace)(const void*, void*) = NULL;           /* CTheScripts::ClearSpaceForMissionEntity(CVector&, CEntity*) */
static void (*pSetRotate)(void*, float, float, float) = NULL;    /* CMatrix::SetRotate(x, y, z) */
static void (*pAutoPlaceOnRoad)(void*) = NULL;                   /* CAutomobile::PlaceOnRoadProperly */
static void (*pBikePlaceOnRoad)(void*) = NULL;                   /* CBike::PlaceOnRoadProperly */
static void* (*pFindPlayerPed)(int) = NULL;                      /* FindPlayerPed */
static void (*pWorldRemove)(void* entity) = NULL;                /* CWorld::Remove(CEntity*) - detach from world */
static void (*pAutoD0)(void* veh) = NULL;                        /* CAutomobile deleting destructor (~ + operator delete) */
static void (*pBikeD0)(void* veh) = NULL;                        /* CBike deleting destructor */
static void* (*pFindPlayerVehicle)(int, bool) = NULL;            /* FindPlayerVehicle - player's current car, NULL on foot */

static const MenuKitAPI* s_api = NULL;

static const int MENU_CARS = 1;

/* Class sizes for CVehicle::operator new, from CCheat::VehicleCheat. */
static const size_t AUTOMOBILE_SIZE = 0xbc8; /* 3016 */
static const size_t BIKE_SIZE       = 0xa18; /* 2584 */

/* Spawn distance in front of the player. The cheat uses colmodel radius + 2
   via CModelInfo; fixed 4.0 units avoids that whole chain. */
static const float SPAWN_DIST = 4.0f;
/* VehicleCheat adds pi/2 to the player heading (float const at
   libGTASA.so+0x710a20). */
static const float HEADING_OFFSET = 1.5707964f;

/* primary/secondary = fixed paint job per car. The ctor assigns a RANDOM
   colour on every construction (CCarCols picker), which made the car
   "change colour" each time it was re-spawned; overwriting the game's
   colour slots (veh+0x574..0x577, written by
   CVehicleModelInfo::ChooseVehicleColour in the ctor) with a fixed pair
   keeps each model identifiably its own colour forever. Indices index
   CVehicleModelInfo::ms_vehicleColourTable (libGTASA+0xb8d818), dumped
   from the game: 0=black, 1=white, 2=light blue, 3=dark red, 6=gold,
   14=silver, 79=navy, 86=green, 115=red. */
struct CarOption { int model; const char* name; bool bike;
                   uint8_t primary; uint8_t secondary; };
static const CarOption kCars[] = {
    { 411, "Infernus", false, 115, 115 },  /* red */
    { 429, "Banshee",  false,   2,   2 },  /* light blue */
    { 415, "Cheetah",  false,   6,   6 },  /* gold */
    { 468, "Sanchez",  true,   86,  86 },  /* green - motorbike -> CBike, everything else CAutomobile */
    { 560, "Sultan",   false,  14,  14 },  /* silver */
    { 541, "Bullet",   false,  79,  79 },  /* navy */
};
static const int kCarCount = (int)(sizeof(kCars) / sizeof(kCars[0]));

static int s_carIndex = 0;
static void* s_spawned = NULL;      /* CVehicle* of our last spawned car */
static bool s_spawnedBike = false;  /* class of s_spawned, for the right D0 */
static void* s_centerHandle = NULL;
static void* s_leftHandle = NULL;
static void* s_rightHandle = NULL;

/* Remove our previously spawned car, if any. Mirrors the game's remove-car
   path: CWorld::Remove + deleting destructor. Never removes the car the
   player is currently driving (that would corrupt the game state). */
static void DespawnCurrent(void)
{
    if(!s_spawned) return;

    if(pFindPlayerVehicle && pFindPlayerVehicle(-1, false) == s_spawned)
    {
        logger->Info("CarSelector: skipping despawn of %s - player is inside it",
                     kCars[s_carIndex].name);
        return;
    }

    logger->Info("CarSelector: despawning previous car");
    pWorldRemove(s_spawned);
    if(s_spawnedBike)
        pBikeD0(s_spawned);
    else
        pAutoD0(s_spawned);
    s_spawned = NULL;
    s_spawnedBike = false;
}

/* Replicate the spawn half of CCheat::VehicleCheat past its abort guards.
   Caller picks the class (bike vs automobile); same sizes, ctors and flag
   writes the cheat uses. The ctor returns the exact CVehicle*, so the
   caller keeps it directly - no pool scan. Model is one of kCars[], all of
   which load synchronously from the same two streaming calls the cheat
   itself makes. */
static void* SpawnManual(const CarOption* car)
{
    int model = car->model;
    bool bike = car->bike;

    pRequestModel(model, 2);
    pLoadAllRequestedModels(false);

    void* veh = pVehicleNew(bike ? BIKE_SIZE : AUTOMOBILE_SIZE);
    if(!veh)
    {
        logger->Error("CarSelector: CVehicle::operator new returned NULL");
        return NULL;
    }
    if(bike) pBikeCtor(veh, model, 1);      /* createdBy = 1 (mission) */
    else     pAutoCtor(veh, model, 1, 1);   /* createdBy = 1, addToWorld = 1 */

    /* Post-ctor flag writes, same as the cheat */
    *(uint8_t*)((uintptr_t)veh + 0x5a) = 0x20u | (*(uint8_t*)((uintptr_t)veh + 0x5a) & 7u);
    *(uint32_t*)((uintptr_t)veh + 0x684) = 1u;
    if(bike) *(uint8_t*)((uintptr_t)veh + 0x800) |= 0x10u;

    /* Fixed paint job: the game's colour slots are written at +0x574..0x577
       by the ctor (ChooseVehicleColour), overwriting them makes the spawn
       deterministic so the car never "changes colour" on re-spawn. */
    *(uint8_t*)((uintptr_t)veh + 0x574) = car->primary;
    *(uint8_t*)((uintptr_t)veh + 0x575) = car->secondary;

    /* Spawn point: SPAWN_DIST units along the player's forward vector */
    void* ped = pFindPlayerPed(-1);
    if(!ped)
    {
        logger->Error("CarSelector: FindPlayerPed returned NULL, aborting spawn");
        return NULL;
    }
    float* pedMtx = *(float**)((uintptr_t)ped + 0x18); /* placement matrix */
    if(!pedMtx) pedMtx = (float*)((uintptr_t)ped + 0x8); /* embedded fallback */
    float sx = pedMtx[12] + pedMtx[4] * SPAWN_DIST; /* pos + forward */
    float sy = pedMtx[13] + pedMtx[5] * SPAWN_DIST;
    float sz = pedMtx[14] + pedMtx[6] * SPAWN_DIST;

    /* Move the new car there, face player heading + 90 deg; SetRotate only
       writes the rotation basis, so re-apply pos after (same as the cheat) */
    float* vehMtx = *(float**)((uintptr_t)veh + 0x18);
    if(!vehMtx) vehMtx = (float*)((uintptr_t)veh + 0x8);
    if(vehMtx)
    {
        float heading = *(float*)((uintptr_t)ped + 0x6c4) + HEADING_OFFSET;
        vehMtx[12] = sx; vehMtx[13] = sy; vehMtx[14] = sz;
        pSetRotate(vehMtx, 0.0f, 0.0f, heading);
        vehMtx[12] = sx; vehMtx[13] = sy; vehMtx[14] = sz;
    }

    pWorldAdd(veh);
    pClearSpace((const void*)&sx, (void*)veh);
    if(bike) pBikePlaceOnRoad(veh);
    else     pAutoPlaceOnRoad(veh);
    return veh;
}

static void SpawnSelected(void)
{
    const CarOption* car = &kCars[s_carIndex];

    DespawnCurrent();
    void* v = SpawnManual(car);
    if(v)
    {
        s_spawned = v;
        s_spawnedBike = car->bike;
        logger->Info("CarSelector: spawned %s (%d) veh 0x%p",
                     car->name, car->model, v);
    }
    else
        logger->Error("CarSelector: spawn failed for %s (%d)", car->name, car->model);
}

static void OnRootButton(void* userdata)
{
    logger->Info("CarSelector: opening car menu (menu %d)", MENU_CARS);
    if(!s_api || s_api->OpenMenu(MENU_CARS) != 0)
        logger->Error("CarSelector: could not open menu %d", MENU_CARS);
}

static void OnArrow(void* userdata)
{
    int dir = (int)(intptr_t)userdata; /* -1 left, +1 right */
    s_carIndex = (s_carIndex + dir + kCarCount) % kCarCount;
    logger->Info("CarSelector: arrow %s -> %s (%d)",
                 dir < 0 ? "left" : "right",
                 kCars[s_carIndex].name, kCars[s_carIndex].model);
    SpawnSelected();
}

static void OnSelect(void* userdata)
{
    const CarOption* car = &kCars[s_carIndex];
    logger->Info("CarSelector: select %s (%d)", car->name, car->model);
    SpawnSelected();
    if(s_api) s_api->CloseMenu(); /* spawn and get back to the root */
}

ON_MOD_LOAD()
{
    logger->SetTag("CarSelector");

    uintptr_t pGameHandle = aml->GetLib("libGTASA.so");
    pRequestModel         = (void(*)(int,int))aml->GetSym(pGameHandle, "_ZN10CStreaming12RequestModelEii");
    pLoadAllRequestedModels= (void(*)(bool))aml->GetSym(pGameHandle, "_ZN10CStreaming22LoadAllRequestedModelsEb");
    pVehicleNew           = (void*(*)(size_t))aml->GetSym(pGameHandle, "_ZN8CVehiclenwEm");
    pAutoCtor             = (void(*)(void*,int,uint8_t,uint8_t))aml->GetSym(pGameHandle, "_ZN11CAutomobileC1Eihh");
    pBikeCtor             = (void(*)(void*,int,uint8_t))aml->GetSym(pGameHandle, "_ZN5CBikeC1Eih");
    pWorldAdd             = (void(*)(void*))aml->GetSym(pGameHandle, "_ZN6CWorld3AddEP7CEntity");
    pClearSpace           = (void(*)(const void*,void*))aml->GetSym(pGameHandle, "_ZN11CTheScripts26ClearSpaceForMissionEntityERK7CVectorP7CEntity");
    pSetRotate            = (void(*)(void*,float,float,float))aml->GetSym(pGameHandle, "_ZN7CMatrix9SetRotateEfff");
    pAutoPlaceOnRoad      = (void(*)(void*))aml->GetSym(pGameHandle, "_ZN11CAutomobile19PlaceOnRoadProperlyEv");
    pBikePlaceOnRoad      = (void(*)(void*))aml->GetSym(pGameHandle, "_ZN5CBike19PlaceOnRoadProperlyEv");
    pFindPlayerPed        = (void*(*)(int))aml->GetSym(pGameHandle, "_Z13FindPlayerPedi");
    pWorldRemove          = (void(*)(void*))aml->GetSym(pGameHandle, "_ZN6CWorld6RemoveEP7CEntity");
    pAutoD0               = (void(*)(void*))aml->GetSym(pGameHandle, "_ZN11CAutomobileD0Ev");
    pBikeD0               = (void(*)(void*))aml->GetSym(pGameHandle, "_ZN5CBikeD0Ev");
    pFindPlayerVehicle    = (void*(*)(int,bool))aml->GetSym(pGameHandle, "_Z17FindPlayerVehicleib");
    if(!pRequestModel || !pLoadAllRequestedModels || !pVehicleNew ||
       !pAutoCtor || !pBikeCtor || !pWorldAdd || !pClearSpace ||
       !pSetRotate || !pAutoPlaceOnRoad || !pBikePlaceOnRoad ||
       !pFindPlayerPed || !pWorldRemove || !pAutoD0 || !pBikeD0 ||
       !pFindPlayerVehicle)
    {
        logger->Error("CarSelector: missing game symbol (stream=%p loadall=%p new=%p autoctor=%p bikeCtor=%p add=%p clear=%p rotate=%p autoroad=%p bikeroad=%p ped=%p rem=%p autod0=%p biked0=%p fpv=%p)",
                      pRequestModel, pLoadAllRequestedModels, pVehicleNew,
                      pAutoCtor, pBikeCtor, pWorldAdd, pClearSpace,
                      pSetRotate, pAutoPlaceOnRoad, pBikePlaceOnRoad,
                      pFindPlayerPed, pWorldRemove, pAutoD0, pBikeD0,
                      pFindPlayerVehicle);
        return;
    }

    s_api = MenuKit_GetAPI(aml);
    if(!s_api)
    {
        logger->Error("CarSelector: MenuKit framework not loaded (load AML_PSDK_MenuKit64 first)");
        return;
    }
    if(s_api->version < 4)
    {
        logger->Error("CarSelector: MenuKit API v4 required (got v%u)", s_api->version);
        s_api = NULL;
        return;
    }

    /* "shoot" is a REAL texture name in the game's image DB (the native attack
       button CWidgetButtonAttackC2 references it at libGTASA.so+0x83d611). A
       bogus name leaves the sprite empty -> invisible button, but the widget
       still keeps its pool id and touch rect, so it stays tappable. Coordinates
       are VIRTUAL 640x448: left/right = (OriginX +/- ScaleX) * screenW/640,
       top/bottom = (OriginY +/- ScaleY) * screenH/448 (CWidget::Update). */

    /* ROOT group (menu 0): "CARS" button, BOTTOM-CENTER (320,400). Deliberately
       NOT at the virtual center (320,224): the menu-1 confirm button lives at
       the center, and a root button under the same physical point would catch
       the confirm tap's release and reopen the menu right after CloseMenu —
       the arrows visibly never disappeared (observed: "select" then "opening
       car menu" back-to-back). Moving the opener away keeps a confirmed
       selection closed. */
    void* root = s_api->AddButton(0, "shoot", 320.0f, 380.0f, 60.0f,
                                  OnRootButton, NULL, NULL);
    if(!root) logger->Error("CarSelector: could not add root widget");
    else
    {
        s_api->SetText(root, "CARS");
        logger->Info("CarSelector: root button labeled CARS");
    }

    /* MENU 1: arrows at the sides, invisible select button in the middle.
       The "<" and ">" glyphs come from the embedded 5x7 font (SetText
       ASCII). No name/label is shown anywhere ("solo las flechas"). */
    s_leftHandle = s_api->AddButton(MENU_CARS, "shoot", 190.0f, 224.0f, 40.0f,
                                    OnArrow, (void*)(intptr_t)-1, NULL);
    s_centerHandle = s_api->AddButton(MENU_CARS, "menu_kit_nonexistent",
                                      320.0f, 224.0f, 60.0f,
                                      OnSelect, NULL, NULL);
    s_rightHandle = s_api->AddButton(MENU_CARS, "shoot", 450.0f, 224.0f, 40.0f,
                                     OnArrow, (void*)(intptr_t)+1, NULL);
    if(!s_leftHandle || !s_centerHandle || !s_rightHandle)
        logger->Error("CarSelector: could not add menu buttons");
    else
    {
        s_api->SetText(s_leftHandle, "<");
        s_api->SetText(s_rightHandle, ">");
        logger->Info("CarSelector: car menu ready (%d cars), center invisible", kCarCount);
    }
}