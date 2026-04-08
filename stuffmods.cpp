#include <jni.h>
#include <dlfcn.h>
#include <pthread.h>
#include <unistd.h>
#include <android/log.h>
#include <cstdio>
#include <cstring>
#include <sys/mman.h>

#define TAG "stuffmods"
#define LOG(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)

static void* base = nullptr;
static void* rva(uint64_t o){ return (uint8_t*)base+o; }
template<typename T> static T    rp(void* p,size_t o){return *(T*)((uint8_t*)p+o);}
template<typename T> static void wp(void* p,size_t o,T v){*(T*)((uint8_t*)p+o)=v;}

// ── IL2CPP ────────────────────────────────────────────────────────────────────
static void*(*_strNew)(const char*) = nullptr;
static void*(*_classGetType)(void*) = nullptr;
static void*(*_typeGetObject)(void*)= nullptr;
static void*(*_domainGet)()         = nullptr;
static void*(*_asmOpen)(void*,const char*) = nullptr;
static void*(*_asmGetImage)(void*)  = nullptr;
static void*(*_classFromName)(void*,const char*,const char*) = nullptr;

// ── Unity RVA function pointers ───────────────────────────────────────────────
typedef void*(*CameraGetMain_t)();
typedef void*(*GOCreatePrimitive_t)(int);
typedef void*(*GOAddComponent_t)(void*,void*); // AddComponent(Type)
typedef void*(*CompGetTransform_t)(void*);
typedef void (*TransSetParent_t)(void*,void*,bool);
typedef void (*TransSetLocalPos_t)(void*,float,float,float); // Vector3 by value ARM64
typedef void (*TransSetLocalScale_t)(void*,float,float,float);
typedef void (*TransSetLocalEuler_t)(void*,float,float,float);
typedef void (*TextMeshSetText_t)(void*,void*);
typedef void (*GOSetActive_t)(void*,bool);
typedef void (*Destroy_t)(void*);
typedef void*(*GOGetComponent_t)(void*,void*);

static CameraGetMain_t    _camMain   = nullptr;
static GOCreatePrimitive_t _createPrim= nullptr;
static GOAddComponent_t   _addComp   = nullptr;
static CompGetTransform_t _getTransform = nullptr;
static TransSetParent_t   _setParent  = nullptr;
static TransSetLocalPos_t _setLocalPos= nullptr;
static TransSetLocalScale_t _setLocalScale = nullptr;
static TextMeshSetText_t  _setText   = nullptr;
static GOSetActive_t      _setActive = nullptr;
static Destroy_t          _destroy   = nullptr;
static GOGetComponent_t   _getComp   = nullptr;
static void*              _meshRendClass = nullptr;

// ── Mod state ────────────────────────────────────────────────────────────────
static bool g_infAmmo   = true;
static bool g_rapidFire = true;
static bool g_maxCurr   = true;

struct Item { const char* name; bool* val; };
static Item g_items[] = {
    {"Inf Ammo",     &g_infAmmo  },
    {"Rapid Fire",   &g_rapidFire},
    {"Max Currency", &g_maxCurr  },
};
static const int N = 3;

// ── TextMesh HUD objects ──────────────────────────────────────────────────────
static void* g_lineGOs[N+2] = {}; // title + N items + hint
static bool  g_hudBuilt = false;
static void* _textMeshClass = nullptr;

static void* createTextLine(void* camTransform, float localX, float localY, float localZ, float scale) {
    // Create empty GO, add TextMesh, parent to camera
    void* go = _createPrim(3); // Cube primitive
    if (!go) return nullptr;
    // Disable renderer so we just have the object
    // Actually TextMesh doesn't need a primitive - but CreatePrimitive is easiest
    // The mesh renderer will just be invisible since TextMesh draws its own mesh
    // Get its transform and parent to camera
    void* t = _getTransform(go);
    if (!t) return nullptr;
    _setParent(t, camTransform, false);
    _setLocalPos(t, localX, localY, localZ);
    _setLocalScale(t, scale, scale, scale);

    // Add TextMesh component - need System.Type object for TextMesh class
    // Remove the cube's MeshRenderer and Collider so only TextMesh is visible
    if (_destroy && _getComp && _meshRendClass) {
        void* rendType = _classGetType(_meshRendClass);
        void* rendTypeObj = _typeGetObject(rendType);
        void* rend = _getComp(go, rendTypeObj);
        if (rend) _destroy(rend);
    }
    void* typePtr = _classGetType(_textMeshClass);
    void* typeObj = _typeGetObject(typePtr);
    void* tm = _addComp(go, typeObj);
    return tm; // TextMesh component
}

static bool buildHUD(void* cgmSelf) {
    if (!_camMain||!_createPrim||!_addComp||!_getTransform||
        !_setParent||!_setLocalPos||!_setLocalScale||!_setText||!_textMeshClass) {
        LOG("buildHUD: missing function pointers");
        return false;
    }
    void* cam = _camMain();
    if (!cam) { LOG("buildHUD: no main camera"); return false; }
    void* camT = _getTransform(cam);
    if (!camT) { LOG("buildHUD: no cam transform"); return false; }

    // Place text in top-right of camera view
    // localZ = 0.5 (50cm in front), localX = +0.15 (right), localY starts at top
    float z = 0.5f, x = 0.18f, startY = 0.13f, lineH = 0.028f;
    float scale = 0.003f;

    for (int i = 0; i < N+2; i++) {
        float y = startY - i * lineH;
        g_lineGOs[i] = createTextLine(camT, x, y, z, scale);
        if (!g_lineGOs[i]) { LOG("buildHUD: failed line %d", i); return false; }
    }

    // Set title
    _setText(g_lineGOs[0], _strNew("=== StuffMods ==="));
    // Set hint
    _setText(g_lineGOs[N+1], _strNew("X=next  A=toggle"));

    LOG("buildHUD OK");
    return true;
}

static void updateHUD() {
    for (int i = 0; i < N; i++) {
        if (!g_lineGOs[i+1]) continue;
        char buf[64];
        snprintf(buf, sizeof(buf), "%s [%s]", g_items[i].name, *g_items[i].val ? "ON" : "OFF");
        _setText(g_lineGOs[i+1], _strNew(buf));
    }
}

// ── Hook infra ────────────────────────────────────────────────────────────────
typedef int(*DobbyHook_t)(void*,void*,void**);
static DobbyHook_t _dobby = nullptr;
static bool hook(void* tgt, void* rep, void** orig) {
    if (!_dobby){
        _dobby=(DobbyHook_t)dlsym(RTLD_DEFAULT,"DobbyHook");
        if(!_dobby)_dobby=(DobbyHook_t)dlsym(RTLD_DEFAULT,"A64HookFunction");
    }
    if (_dobby){ int r=_dobby(tgt,rep,orig); LOG("Dobby %p r=%d",tgt,r); return r==0; }
    uintptr_t pg=(uintptr_t)tgt&~0xFFFull;
    mprotect((void*)pg,0x2000,PROT_READ|PROT_WRITE|PROT_EXEC);
    if(orig){
        uint8_t* tr=(uint8_t*)mmap(nullptr,32,PROT_READ|PROT_WRITE|PROT_EXEC,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
        if(tr!=MAP_FAILED){
            memcpy(tr,tgt,16);
            uint32_t j[4]={0x58000051,0xD61F0220,0,0};
            uintptr_t c=(uintptr_t)tgt+16;memcpy(&j[2],&c,8);
            memcpy(tr+16,j,16);__builtin___clear_cache((char*)tr,(char*)tr+32);
            *orig=tr;
        }
    }
    uint32_t p[4]={0x58000051,0xD61F0220,0,0};
    memcpy(&p[2],&rep,8);memcpy(tgt,p,16);
    __builtin___clear_cache((char*)tgt,(char*)tgt+16);
    return true;
}

// ── Weapon hooks ──────────────────────────────────────────────────────────────
typedef void(*ShamUpd_t)(void*); static ShamUpd_t _shamOrig=nullptr;
static void shamHook(void* s){
    if(_shamOrig)_shamOrig(s); if(!s)return;
    if(g_infAmmo){int32_t m=rp<int32_t>(s,0x88);if(m>0&&m<9999){wp<int32_t>(s,0x98,m);wp<uint8_t>(s,0x9C,0);}}
    if(g_rapidFire){wp<float>(s,0x8C,0.f);wp<uint8_t>(s,0x9C,0);}
}
typedef void(*ShotUpd_t)(void*); static ShotUpd_t _shotOrig=nullptr;
static void shotHook(void* s){
    if(_shotOrig)_shotOrig(s); if(!s)return;
    if(g_infAmmo||g_rapidFire){wp<uint8_t>(s,0x40,1);wp<uint8_t>(s,0x98,0);wp<int32_t>(s,0x9C,0);}
}

// ── CGM.LateUpdate hook ───────────────────────────────────────────────────────
typedef void(*CGMLate_t)(void*); static CGMLate_t _cgmOrig=nullptr;
static int g_frame=0;

static void cgmHook(void* self){
    if(_cgmOrig)_cgmOrig(self);
    g_frame++;

    if(g_maxCurr&&g_frame%300==0){
        wp<int32_t>(self,0xD0,999999);
        wp<int32_t>(self,0xE4,999999);
    }

    // Build HUD once after a short delay
    if(!g_hudBuilt&&g_frame==60){
        g_hudBuilt=buildHUD(self);
    }
    if(g_hudBuilt&&g_frame%30==0) updateHUD();
}

// ── Setup ─────────────────────────────────────────────────────────────────────
static void* setup(void*){
    sleep(20);

    char path[512]={};
    {FILE* m=fopen("/proc/self/maps","r");char l[512];
    while(m&&fgets(l,sizeof(l),m)){
        if(strstr(l,"libil2cpp.so")&&strstr(l,"r-xp")){
            uint64_t b=0;sscanf(l,"%llx",(unsigned long long*)&b);base=(void*)b;
            char* p=strrchr(l,' ');if(!p)p=strrchr(l,'\t');
            if(p){strncpy(path,p+1,sizeof(path)-1);path[strcspn(path,"\n")]=0;}
            LOG("base=%p",base);break;
        }
    }if(m)fclose(m);}
    if(!base){LOG("no libil2cpp");return nullptr;}

    // Resolve exports
    void* h=dlopen(path[0]?path:"libil2cpp.so",RTLD_NOLOAD|RTLD_NOW|RTLD_GLOBAL);
    void* src=h?h:RTLD_DEFAULT;
    _strNew      =(decltype(_strNew))     dlsym(src,"il2cpp_string_new");
    _classGetType=(decltype(_classGetType))dlsym(src,"il2cpp_class_get_type");
    _typeGetObject=(decltype(_typeGetObject))dlsym(src,"il2cpp_type_get_object");
    _domainGet   =(decltype(_domainGet))  dlsym(src,"il2cpp_domain_get");
    _asmOpen     =(decltype(_asmOpen))    dlsym(src,"il2cpp_domain_assembly_open");
    _asmGetImage =(decltype(_asmGetImage))dlsym(src,"il2cpp_assembly_get_image");
    _classFromName=(decltype(_classFromName))dlsym(src,"il2cpp_class_from_name");
    LOG("strNew=%p classGetType=%p",(void*)_strNew,(void*)_classGetType);

    // Unity function pointers via RVA
    _camMain     =(CameraGetMain_t)   rva(0x4682E6C);
    _createPrim  =(GOCreatePrimitive_t)rva(0x46B3318);
    _addComp     =(GOAddComponent_t)  rva(0x46B36CC);
    _getTransform=(CompGetTransform_t)rva(0x46B07F0);
    _setParent   =(TransSetParent_t)  rva(0x46BD8DC);
    _setLocalPos =(TransSetLocalPos_t)rva(0x46BC5CC);
    _setLocalScale=(TransSetLocalScale_t)rva(0x46BCE28);
    _setText     =(TextMeshSetText_t) rva(0x449D348);
    _destroy     =(Destroy_t)          rva(0x46B7550);
    _getComp     =(GOGetComponent_t)   rva(0x46B08E0);

    // Find TextMesh class for AddComponent
    if(_domainGet&&_asmOpen&&_asmGetImage&&_classFromName){
        void* dom=nullptr;
        for(int r=0;r<15&&!dom;r++){dom=_domainGet();if(!dom)sleep(1);}
        if(dom){
            // Get all assemblies and scan for TextMesh
            typedef void**(*GetAssemblies_t)(void*,size_t*);
            auto _getAsms=(GetAssemblies_t)dlsym(src,"il2cpp_domain_get_assemblies");
            if(_getAsms){
                size_t asmCount=0;
                void** allAsms=_getAsms(dom,&asmCount);
                LOG("scanning %zu assemblies for TextMesh",(size_t)asmCount);
                for(size_t a=0;a<asmCount;a++){
                    void* img=_asmGetImage(allAsms[a]);if(!img)continue;
                    void* cls=_classFromName(img,"","TextMeshPro");
                    if(cls){_textMeshClass=cls;LOG("TextMeshPro found in asm index %zu",(size_t)a);}
                    void* mrCls=_classFromName(img,"","MeshRenderer");
                    if(mrCls&&!_meshRendClass)_meshRendClass=mrCls;
                    if(_textMeshClass&&_meshRendClass)break;
                }
            }
        }
    }
    if(!_textMeshClass) LOG("WARNING: TextMeshPro class not found");

    // Install hooks
    sleep(2);
    hook(rva(0x20AFB28),(void*)cgmHook, (void**)&_cgmOrig);
    sleep(1);
    hook(rva(0x20CA130),(void*)shamHook,(void**)&_shamOrig);
    hook(rva(0x20E0304),(void*)shotHook,(void**)&_shotOrig);

    LOG("stuffmods ready");
    return nullptr;
}

extern "C" JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm,void*){
    (void)vm;
    LOG("stuffmods loaded!");
    pthread_t t;pthread_create(&t,nullptr,setup,nullptr);pthread_detach(t);
    return JNI_VERSION_1_6;
}
