#include <fbxvpch.h>

#include <App.h>
#include <AppSurfaceSdlVk.h>
#include <Swapchain.Vulkan.h>
#include <Input.h>
#include <NuklearSdlVk.h>
#include <DebugRendererVk.h>

#include <AppState.h>

#include <Scene.h>

#include <EmbeddedShaderPreprocessor.h>

namespace apemode {
    using namespace apemodevk;
}

using namespace apemode;

static const uint32_t kMaxFrames = Swapchain::kMaxImgs;
static apemode::AppContent * gAppContent = nullptr;


struct BasicCamera {
    mathfu::mat4 ViewMatrix;
    mathfu::mat4 ProjMatrix;
};

namespace mathfu {

    template <typename T>
    T NormalizedSafe(const T& V, float SafeEps = 1e-5f) {
        const float length = V.Length();
        const float invLength = 1.0f / (length + SafeEps);

        return V * invLength;
    }

    template <typename T>
    T NormalizedSafeAndLength(const T& V, float Length float SafeEps = 1e-5f) {
        const float length = V.Length();
        const float invLength = 1.0f / (length + SafeEps);

        Length = length;
        return V * invLength;
    }

}

struct ModelViewCameraController {

    mathfu::vec3 Target;
    mathfu::vec3 Position;
    mathfu::vec3 TargetDst;
    mathfu::vec3 PositionDst;
    mathfu::vec2 Orbit;
    float NearPlane;
    float FarPlane;

    ModelViewCameraController() {
        NearPlane = 0.1f;
        FarPlane = 1000.0f;
        Reset();
    }


    void Reset() {
        Target = { 0, 0, 0 };
        Position = { 0, 0, -300 };
        TargetDst = { 0, 0, 0 };
        PositionDst = { 0, 0, -300 };
        Orbit = { 0, 0 };
    }

    mathfu::mat4 mtxLookAt() {
        //bx::mtxLookAtRh( _outViewMtx, Position, Target );
        //bx::mtxLookAt(_outViewMtx, Position, Target);
        return mathfu::mat4::LookAt(Target, Position, { 0, 1, 0 }, 1);
    }

    void orbit(mathfu::vec2 dxdy) {
        Orbit += dxdy;
    }

    void dolly(float _dz) {
        float toTargetLen;
        const mathfu::vec3 toTarget = TargetDst - PositionDst;
        const mathfu::vec3 toTargetNorm = mathfu::NormalizedSafeAndLength(toTarget, toTargetLen);

        float delta = toTargetLen * _dz;
        float newLen = toTargetLen + delta;
        if ((NearPlane < newLen || _dz < 0.0f) && (newLen < FarPlane || _dz > 0.0f)) {
            PositionDst += toTargetNorm * delta;
        }
    }

    void consumeOrbit(float _amount) {
        mathfu::vec2 consume = Orbit * _amount;
        Orbit -= consume;

        float toPosLen;
        const mathfu::vec3 toPos = Position - Target;
        const mathfu::vec3 toPosNorm = mathfu::NormalizedSafeAndLength(toPos, toPosLen);

        float ll[2];
        latLongFromVec(ll[0], ll[1], toPosNorm);
        ll[0] += consume[0];
        ll[1] -= consume[1];
        ll[1] = bx::fclamp(ll[1], 0.02f, 0.98f);

        float tmp[3];
        vecFromLatLong(tmp, ll[0], ll[1]);

        float diff[3];
        diff[0] = (tmp[0] - toPosNorm[0]) * toPosLen;
        diff[1] = (tmp[1] - toPosNorm[1]) * toPosLen;
        diff[2] = (tmp[2] - toPosNorm[2]) * toPosLen;

        Position[0] += diff[0];
        Position[1] += diff[1];
        Position[2] += diff[2];
        PositionDst[0] += diff[0];
        PositionDst[1] += diff[1];
        PositionDst[2] += diff[2];
    }

    void update(float _dt) {
        const float amount = bx::fmin(_dt / 0.12f, 1.0f);

        consumeOrbit(amount);

        Target[0] = bx::flerp(Target[0], TargetDst[0], amount);
        Target[1] = bx::flerp(Target[1], TargetDst[1], amount);
        Target[2] = bx::flerp(Target[2], TargetDst[2], amount);
        Position[0] = bx::flerp(Position[0], PositionDst[0], amount);
        Position[1] = bx::flerp(Position[1], PositionDst[1], amount);
        Position[2] = bx::flerp(Position[2], PositionDst[2], amount);
    }

    void envViewMtx(float* _mtx) {
        const float toTarget[3] = {
            Target[0] - Position[0],
            Target[1] - Position[1],
            Target[2] - Position[2],
        };

        const float toTargetLen = bx::vec3Length(toTarget);
        const float invToTargetLen = 1.0f / (toTargetLen + FLT_MIN);

        const float toTargetNorm[3] = {
            toTarget[0] * invToTargetLen,
            toTarget[1] * invToTargetLen,
            toTarget[2] * invToTargetLen,
        };

        float tmp[3];
        const float fakeUp[3] = { 0.0f, 1.0f, 0.0f };

        float right[3];
        bx::vec3Cross(tmp, fakeUp, toTargetNorm);
        bx::vec3Norm(right, tmp);

        float up[3];
        bx::vec3Cross(tmp, toTargetNorm, right);
        bx::vec3Norm(up, tmp);

        _mtx[0] = right[0];
        _mtx[1] = right[1];
        _mtx[2] = right[2];
        _mtx[3] = 0.0f;
        _mtx[4] = up[0];
        _mtx[5] = up[1];
        _mtx[6] = up[2];
        _mtx[7] = 0.0f;
        _mtx[8] = toTargetNorm[0];
        _mtx[9] = toTargetNorm[1];
        _mtx[10] = toTargetNorm[2];
        _mtx[11] = 0.0f;
        _mtx[12] = 0.0f;
        _mtx[13] = 0.0f;
        _mtx[14] = 0.0f;
        _mtx[15] = 1.0f;
    }

    static inline void vecFromLatLong(float _vec[3], float _u, float _v) {
        const float phi = _u * 2.0f * bx::pi;
        const float theta = _v * bx::pi;

        const float st = bx::fsin(theta);
        const float sp = bx::fsin(phi);
        const float ct = bx::fcos(theta);
        const float cp = bx::fcos(phi);

        _vec[0] = -st * sp;
        _vec[1] = ct;
        _vec[2] = -st * cp;
    }

    static inline void latLongFromVec(float& _u, float& _v, const float _vec[3]) {
        const float phi = atan2f(_vec[0], _vec[2]);
        const float theta = acosf(_vec[1]);

        _u = (bx::pi + phi) * bx::invPi * 0.5f;
        _v = theta * bx::invPi;
    }
};

const VkFormat sDepthFormat = VK_FORMAT_D16_UNORM;

class apemode::AppContent {
public:

    nk_color diffColor;
    nk_color specColor;
    uint32_t width      = 0;
    uint32_t height     = 0;
    uint32_t resetFlags = 0;
    uint32_t envId      = 0;
    uint32_t sceneId    = 0;
    uint32_t maskId     = 0;
    Scene*   scenes[ 2 ];

    NuklearSdlBase* Nk = nullptr;
    DebugRendererVk * Dbg = nullptr;

    uint32_t FrameCount = 0;
    uint32_t FrameIndex = 0;
    uint64_t FrameId    = 0;

    uint32_t BackbufferIndices[ kMaxFrames ] = {0};

    std::unique_ptr< DescriptorPool >      pDescPool;
    TDispatchableHandle< VkRenderPass >    hNkRenderPass;
    TDispatchableHandle< VkFramebuffer >   hNkFramebuffers[ kMaxFrames ];
    TDispatchableHandle< VkCommandPool >   hCmdPool[ kMaxFrames ];
    TDispatchableHandle< VkCommandBuffer > hCmdBuffers[ kMaxFrames ];
    TDispatchableHandle< VkFence >         hFences[ kMaxFrames ];
    TDispatchableHandle< VkSemaphore >     hPresentCompleteSemaphores[ kMaxFrames ];
    TDispatchableHandle< VkSemaphore >     hRenderCompleteSemaphores[ kMaxFrames ];

    TDispatchableHandle< VkRenderPass >    hDbgRenderPass;
    TDispatchableHandle< VkFramebuffer >   hDbgFramebuffers[ kMaxFrames ];
    TDispatchableHandle< VkImage >         hDepthImgs[ kMaxFrames ];
    TDispatchableHandle< VkImageView >     hDepthImgViews[ kMaxFrames ];
    TDispatchableHandle< VkDeviceMemory >  hDepthImgMemory[ kMaxFrames ];

    AppContent()  {
    }

    ~AppContent() {
    }
};

App::App( ) : appState(new AppState()) {
    if (nullptr != appState)
        appState->appOptions->add_options("vk")
            ("renderdoc", "Adds renderdoc layer to device layers")
            ("vkapidump", "Adds api dump layer to vk device layers")
            ("vktrace", "Adds vktrace layer to vk device layers");
}

App::~App( ) {
    if (nullptr != appState)
        delete appState;

    if ( nullptr != appContent )
        delete appContent;
}

IAppSurface* App::CreateAppSurface( ) {
    return new AppSurfaceSdlVk( );
}

bool App::Initialize( int Args, char* ppArgs[] ) {

    if (appState && appState->appOptions)
        appState->appOptions->parse(Args, ppArgs);

    if ( AppBase::Initialize( Args, ppArgs ) ) {

        if ( nullptr == appContent )
            appContent = new AppContent( );

        totalSecs = 0.0f;

        auto appSurface = GetSurface();
        if (appSurface->GetImpl() != kAppSurfaceImpl_SdlVk)
            return false;

        auto appSurfaceVk = (AppSurfaceSdlVk*)appSurface;
        if ( auto swapchain = appSurfaceVk->pSwapchain.get( ) ) {
            appContent->FrameCount = swapchain->ImgCount;
            appContent->FrameIndex = 0;
            appContent->FrameId    = 0;
           
            OnResized();

            for (uint32_t i = 0; i < appContent->FrameCount; ++i) {
                VkCommandPoolCreateInfo cmdPoolCreateInfo;
                InitializeStruct( cmdPoolCreateInfo );
                cmdPoolCreateInfo.flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
                cmdPoolCreateInfo.queueFamilyIndex = appSurfaceVk->pCmdQueue->QueueFamilyId;

                if ( false == appContent->hCmdPool[ i ].Recreate( *appSurfaceVk->pNode, cmdPoolCreateInfo ) ) {
                    DebugBreak( );
                    return false;
                }

                VkCommandBufferAllocateInfo cmdBufferAllocInfo;
                InitializeStruct( cmdBufferAllocInfo );
                cmdBufferAllocInfo.commandPool        = appContent->hCmdPool[ i ];
                cmdBufferAllocInfo.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
                cmdBufferAllocInfo.commandBufferCount = 1;

                if ( false == appContent->hCmdBuffers[ i ].Recreate( *appSurfaceVk->pNode, cmdBufferAllocInfo ) ) {
                    DebugBreak( );
                    return false;
                }

                VkFenceCreateInfo fenceCreateInfo;
                InitializeStruct( fenceCreateInfo );
                fenceCreateInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;

                if ( false == appContent->hFences[ i ].Recreate( *appSurfaceVk->pNode, fenceCreateInfo ) ) {
                    DebugBreak( );
                    return false;
                }

                VkSemaphoreCreateInfo semaphoreCreateInfo;
                InitializeStruct( semaphoreCreateInfo );
                if ( false == appContent->hPresentCompleteSemaphores[ i ].Recreate( *appSurfaceVk->pNode, semaphoreCreateInfo ) ||
                     false == appContent->hRenderCompleteSemaphores[ i ].Recreate( *appSurfaceVk->pNode, semaphoreCreateInfo ) ) {
                    DebugBreak( );
                    return false;
                }
            }
        }

        appContent->pDescPool = std::move(std::make_unique< DescriptorPool >());
        if (false == appContent->pDescPool->RecreateResourcesFor(*appSurfaceVk->pNode, 256, 256, 256, 256, 256, 256, 256, 256, 256, 256, 256, 256 )) {
            DebugBreak();
            return false;
        }

        appContent->Nk = new NuklearSdlVk();

        NuklearSdlVk::InitParametersVk initParamsNk;
        initParamsNk.pAlloc          = nullptr;
        initParamsNk.pDevice         = *appSurfaceVk->pNode;
        initParamsNk.pPhysicalDevice = *appSurfaceVk->pNode;
        initParamsNk.pRenderPass     = appContent->hDbgRenderPass;
        //initParamsNk.pRenderPass     = appContent->hNkRenderPass;
        initParamsNk.pDescPool       = *appContent->pDescPool;
        initParamsNk.pQueue          = *appSurfaceVk->pCmdQueue;
        initParamsNk.QueueFamilyId   = appSurfaceVk->pCmdQueue->QueueFamilyId;

        appContent->Nk->Init( &initParamsNk );

        appContent->Dbg = new DebugRendererVk();

        DebugRendererVk::InitParametersVk initParamsDbg;
        initParamsDbg.pAlloc          = nullptr;
        initParamsDbg.pDevice         = *appSurfaceVk->pNode;
        initParamsDbg.pPhysicalDevice = *appSurfaceVk->pNode;
        initParamsDbg.pRenderPass     = appContent->hDbgRenderPass;
        initParamsDbg.pDescPool       = *appContent->pDescPool;
        initParamsDbg.FrameCount      = appContent->FrameCount;

        appContent->Dbg->RecreateResources(&initParamsDbg);

        // appContent->scenes[ 0 ] = LoadSceneFromFile( "../../../assets/iron-man.fbxp" );
        // appContent->scenes[ 1 ] = LoadSceneFromFile( "../../../assets/kalestra-the-sorceress.fbxp" );
        // appContent->scenes[ 0 ] = LoadSceneFromFile( "../../../assets/Mech6kv3ps.fbxp" );
        // appContent->scenes[ 0 ] = LoadSceneFromFile( "../../../assets/Mech6k_v2.fbxp" );
        // appContent->scenes[ 1 ] = LoadSceneFromFile( "../../../assets/P90_v2.fbxp" );
        // appContent->scenes[ 0 ] = LoadSceneFromFile( "../../../assets/MercedesBenzA45AMG.fbxp" );
        // appContent->scenes[ 1 ] = LoadSceneFromFile( "../../../assets/MercedesBenzSLR.fbxp" );
        // appContent->scenes[ 1 ] = LoadSceneFromFile( "../../../assets/P90.fbxp" );
        // appContent->scenes[ 1 ] = LoadSceneFromFile( "../../../assets/IronMan.fbxp" );
        // appContent->scenes[ 1 ] = LoadSceneFromFile( "../../../assets/Cathedral.fbxp" );
        // appContent->scenes[ 1 ] = LoadSceneFromFile( "../../../assets/Leica1933.fbxp" );
        // appContent->scenes[ 1 ] = LoadSceneFromFile( "../../../assets/UnrealOrb.fbxp" );
        // appContent->scenes[ 1 ] = LoadSceneFromFile( "../../../assets/Artorias.fbxp" );
        // appContent->scenes[ 1 ] = LoadSceneFromFile( "../../../assets/9mm.fbxp" );
        // appContent->scenes[ 1 ] = LoadSceneFromFile( "../../../assets/Knife.fbxp" );
        // appContent->scenes[ 1 ] = LoadSceneFromFile( "../../../assets/mech-m-6k.fbxp" );

        return true;
    }

    return false;
}

bool apemode::App::OnResized( ) {
    if ( auto appSurfaceVk = (AppSurfaceSdlVk*) GetSurface( ) ) {
        if ( auto swapchain = appSurfaceVk->pSwapchain.get( ) ) {
            appContent->width  = appSurfaceVk->GetWidth( );
            appContent->height = appSurfaceVk->GetHeight( );

            VkImageCreateInfo depthImgCreateInfo;
            InitializeStruct( depthImgCreateInfo );
            depthImgCreateInfo.imageType     = VK_IMAGE_TYPE_2D;
            depthImgCreateInfo.format        = sDepthFormat;
            depthImgCreateInfo.extent.width  = swapchain->ColorExtent.width;
            depthImgCreateInfo.extent.height = swapchain->ColorExtent.height;
            depthImgCreateInfo.extent.depth  = 1;
            depthImgCreateInfo.mipLevels     = 1;
            depthImgCreateInfo.arrayLayers   = 1;
            depthImgCreateInfo.samples       = VK_SAMPLE_COUNT_1_BIT;
            depthImgCreateInfo.tiling        = VK_IMAGE_TILING_OPTIMAL;
            depthImgCreateInfo.usage         = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;

            VkImageViewCreateInfo depthImgViewCreateInfo;
            InitializeStruct( depthImgViewCreateInfo );
            depthImgViewCreateInfo.format                          = sDepthFormat;
            depthImgViewCreateInfo.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_DEPTH_BIT;
            depthImgViewCreateInfo.subresourceRange.baseMipLevel   = 0;
            depthImgViewCreateInfo.subresourceRange.levelCount     = 1;
            depthImgViewCreateInfo.subresourceRange.baseArrayLayer = 0;
            depthImgViewCreateInfo.subresourceRange.layerCount     = 1;
            depthImgViewCreateInfo.viewType                        = VK_IMAGE_VIEW_TYPE_2D;

            for ( uint32_t i = 0; i < appContent->FrameCount; ++i ) {
                if ( false == appContent->hDepthImgs[ i ].Recreate( *appSurfaceVk->pNode, *appSurfaceVk->pNode, depthImgCreateInfo ) ) {
                    DebugBreak( );
                    return false;
                }
            }

            for ( uint32_t i = 0; i < appContent->FrameCount; ++i ) {
                auto memoryAllocInfo = appContent->hDepthImgs[ i ].GetMemoryAllocateInfo( VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT );
                if ( false == appContent->hDepthImgMemory[ i ].Recreate( *appSurfaceVk->pNode, memoryAllocInfo ) ) {
                    DebugBreak( );
                    return false;
                }
            }

            for ( uint32_t i = 0; i < appContent->FrameCount; ++i ) {
                if ( false == appContent->hDepthImgs[ i ].BindMemory( appContent->hDepthImgMemory[ i ], 0 ) ) {
                    DebugBreak( );
                    return false;
                }
            }

            for ( uint32_t i = 0; i < appContent->FrameCount; ++i ) {
                depthImgViewCreateInfo.image = appContent->hDepthImgs[ i ];
                if ( false == appContent->hDepthImgViews[ i ].Recreate( *appSurfaceVk->pNode, depthImgViewCreateInfo ) ) {
                    DebugBreak( );
                    return false;
                }
            }

            VkAttachmentDescription colorDepthAttachments[ 2 ];
            InitializeStruct( colorDepthAttachments );

            colorDepthAttachments[ 0 ].format         = swapchain->eColorFormat;
            colorDepthAttachments[ 0 ].samples        = VK_SAMPLE_COUNT_1_BIT;
            colorDepthAttachments[ 0 ].loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;
            colorDepthAttachments[ 0 ].storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
            colorDepthAttachments[ 0 ].stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            colorDepthAttachments[ 0 ].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
            colorDepthAttachments[ 0 ].initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
            colorDepthAttachments[ 0 ].finalLayout    = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

            colorDepthAttachments[ 1 ].format         = sDepthFormat;
            colorDepthAttachments[ 1 ].samples        = VK_SAMPLE_COUNT_1_BIT;
            colorDepthAttachments[ 1 ].loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;
            colorDepthAttachments[ 1 ].storeOp        = VK_ATTACHMENT_STORE_OP_DONT_CARE; // ?
            colorDepthAttachments[ 1 ].stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            colorDepthAttachments[ 1 ].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
            colorDepthAttachments[ 1 ].initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
            colorDepthAttachments[ 1 ].finalLayout    = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

            VkAttachmentReference colorAttachmentRef;
            InitializeStruct( colorAttachmentRef );
            colorAttachmentRef.attachment = 0;
            colorAttachmentRef.layout     = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

            VkAttachmentReference depthAttachmentRef;
            InitializeStruct( depthAttachmentRef );
            depthAttachmentRef.attachment = 1;
            depthAttachmentRef.layout     = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

            VkSubpassDescription subpassNk;
            InitializeStruct( subpassNk );
            subpassNk.pipelineBindPoint    = VK_PIPELINE_BIND_POINT_GRAPHICS;
            subpassNk.colorAttachmentCount = 1;
            subpassNk.pColorAttachments    = &colorAttachmentRef;

            VkSubpassDescription subpassDbg;
            InitializeStruct( subpassDbg );
            subpassDbg.pipelineBindPoint       = VK_PIPELINE_BIND_POINT_GRAPHICS;
            subpassDbg.colorAttachmentCount    = 1;
            subpassDbg.pColorAttachments       = &colorAttachmentRef;
            subpassDbg.pDepthStencilAttachment = &depthAttachmentRef;

            VkRenderPassCreateInfo renderPassCreateInfoNk;
            InitializeStruct( renderPassCreateInfoNk );
            renderPassCreateInfoNk.attachmentCount = 1;
            renderPassCreateInfoNk.pAttachments    = &colorDepthAttachments[ 0 ];
            renderPassCreateInfoNk.subpassCount    = 1;
            renderPassCreateInfoNk.pSubpasses      = &subpassNk;

            VkRenderPassCreateInfo renderPassCreateInfoDbg;
            InitializeStruct( renderPassCreateInfoDbg );
            renderPassCreateInfoDbg.attachmentCount = 2;
            renderPassCreateInfoDbg.pAttachments    = &colorDepthAttachments[ 0 ];
            renderPassCreateInfoDbg.subpassCount    = 1;
            renderPassCreateInfoDbg.pSubpasses      = &subpassDbg;

            if ( false == appContent->hNkRenderPass.Recreate( *appSurfaceVk->pNode, renderPassCreateInfoNk ) ) {
                DebugBreak( );
                return false;
            }

            if ( false == appContent->hDbgRenderPass.Recreate( *appSurfaceVk->pNode, renderPassCreateInfoDbg ) ) {
                DebugBreak( );
                return false;
            }

            VkFramebufferCreateInfo framebufferCreateInfoNk;
            InitializeStruct( framebufferCreateInfoNk );
            framebufferCreateInfoNk.renderPass      = appContent->hNkRenderPass;
            framebufferCreateInfoNk.attachmentCount = 1;
            framebufferCreateInfoNk.width           = swapchain->ColorExtent.width;
            framebufferCreateInfoNk.height          = swapchain->ColorExtent.height;
            framebufferCreateInfoNk.layers          = 1;

            VkFramebufferCreateInfo framebufferCreateInfoDbg;
            InitializeStruct( framebufferCreateInfoDbg );
            framebufferCreateInfoDbg.renderPass      = appContent->hDbgRenderPass;
            framebufferCreateInfoDbg.attachmentCount = 2;
            framebufferCreateInfoDbg.width           = swapchain->ColorExtent.width;
            framebufferCreateInfoDbg.height          = swapchain->ColorExtent.height;
            framebufferCreateInfoDbg.layers          = 1;

            for ( uint32_t i = 0; i < appContent->FrameCount; ++i ) {
                VkImageView attachments[ 1 ] = {swapchain->hImgViews[ i ]};
                framebufferCreateInfoNk.pAttachments = attachments;

                if ( false == appContent->hNkFramebuffers[ i ].Recreate( *appSurfaceVk->pNode, framebufferCreateInfoNk ) ) {
                    DebugBreak( );
                    return false;
                }
            }

            for ( uint32_t i = 0; i < appContent->FrameCount; ++i ) {
                VkImageView attachments[ 2 ] = {swapchain->hImgViews[ i ], appContent->hDepthImgViews[ i ]};
                framebufferCreateInfoDbg.pAttachments = attachments;

                if ( false == appContent->hDbgFramebuffers[ i ].Recreate( *appSurfaceVk->pNode, framebufferCreateInfoDbg ) ) {
                    DebugBreak( );
                    return false;
                }
            }
        }
    }

    return true;
}

void App::OnFrameMove( ) {
    nk_input_begin( &appContent->Nk->Context ); {
        SDL_Event evt;
        while ( SDL_PollEvent( &evt ) )
            appContent->Nk->HandleEvent( &evt );
        nk_input_end( &appContent->Nk->Context );
    }

    AppBase::OnFrameMove( );

    ++appContent->FrameId;
    appContent->FrameIndex = appContent->FrameId % (uint64_t) appContent->FrameCount;
}

void App::Update( float deltaSecs, Input const& inputState ) {
    totalSecs += deltaSecs;

    bool hovered = false;
    bool reset   = false;

    const nk_flags windowFlags
        = NK_WINDOW_BORDER
        | NK_WINDOW_MOVABLE
        | NK_WINDOW_SCALABLE
        | NK_WINDOW_MINIMIZABLE;

    auto ctx = &appContent->Nk->Context;
    float clearColor[ 4 ] = {0};

    if (nk_begin(ctx, "Calculator", nk_rect(10, 10, 180, 250),
        NK_WINDOW_BORDER|NK_WINDOW_NO_SCROLLBAR|NK_WINDOW_MOVABLE))
    {
        static int set = 0, prev = 0, op = 0;
        static const char numbers[] = "789456123";
        static const char ops[] = "+-*/";
        static double a = 0, b = 0;
        static double *current = &a;

        size_t i = 0;
        int solve = 0;
        {int len; char buffer[256];
        nk_layout_row_dynamic(ctx, 35, 1);
        len = snprintf(buffer, 256, "%.2f", *current);
        nk_edit_string(ctx, NK_EDIT_SIMPLE, buffer, &len, 255, nk_filter_float);
        buffer[len] = 0;
        *current = atof(buffer);}

        nk_layout_row_dynamic(ctx, 35, 4);
        for (i = 0; i < 16; ++i) {
            if (i >= 12 && i < 15) {
                if (i > 12) continue;
                if (nk_button_label(ctx, "C")) {
                    a = b = op = 0; current = &a; set = 0;
                } if (nk_button_label(ctx, "0")) {
                    *current = *current*10.0f; set = 0;
                } if (nk_button_label(ctx, "=")) {
                    solve = 1; prev = op; op = 0;
                }
            } else if (((i+1) % 4)) {
                if (nk_button_text(ctx, &numbers[(i/4)*3+i%4], 1)) {
                    *current = *current * 10.0f + numbers[(i/4)*3+i%4] - '0';
                    set = 0;
                }
            } else if (nk_button_text(ctx, &ops[i/4], 1)) {
                if (!set) {
                    if (current != &b) {
                        current = &b;
                    } else {
                        prev = op;
                        solve = 1;
                    }
                }
                op = ops[i/4];
                set = 1;
            }
        }
        if (solve) {
            if (prev == '+') a = a + b;
            if (prev == '-') a = a - b;
            if (prev == '*') a = a * b;
            if (prev == '/') a = a / b;
            current = &a;
            if (set) current = &b;
            b = 0; set = 0;
        }
    }
    nk_end(ctx);

    if ( auto appSurfaceVk = (AppSurfaceSdlVk*) GetSurface( ) ) {
        VkDevice        device                   = *appSurfaceVk->pNode;
        VkQueue         queue                    = *appSurfaceVk->pCmdQueue;
        VkSwapchainKHR  swapchain                = appSurfaceVk->pSwapchain->hSwapchain;
        VkFence         fence                    = appContent->hFences[ appContent->FrameIndex ];
        VkSemaphore     presentCompleteSemaphore = appContent->hPresentCompleteSemaphores[ appContent->FrameIndex ];
        VkSemaphore     renderCompleteSemaphore  = appContent->hRenderCompleteSemaphores[ appContent->FrameIndex ];
        VkCommandPool   cmdPool                  = appContent->hCmdPool[ appContent->FrameIndex ];
        VkCommandBuffer cmdBuffer                = appContent->hCmdBuffers[ appContent->FrameIndex ];

        const uint32_t width  = appSurfaceVk->GetWidth( );
        const uint32_t height = appSurfaceVk->GetHeight( );

        if ( width != appContent->width || height != appContent->height ) {
            CheckedCall( vkDeviceWaitIdle( device ) );
            OnResized( );

        }

        VkFramebuffer framebufferNk = appContent->hNkFramebuffers[ appContent->FrameIndex ];
        VkFramebuffer framebufferDbg = appContent->hDbgFramebuffers[ appContent->FrameIndex ];

        while (true) {
            const auto waitForFencesErrorHandle = vkWaitForFences( device, 1, &fence, VK_TRUE, 100 );
            if ( VK_SUCCESS == waitForFencesErrorHandle ) {
                break;
            } else if ( VK_TIMEOUT == waitForFencesErrorHandle ) {
                continue;
            } else {
                assert( false );
                return;
            }
        }

        CheckedCall( vkAcquireNextImageKHR( device,
                                            swapchain,
                                            UINT64_MAX,
                                            presentCompleteSemaphore,
                                            VK_NULL_HANDLE,
                                            &appContent->BackbufferIndices[ appContent->FrameIndex ] ) );

        CheckedCall( vkResetCommandPool( device, cmdPool, 0 ) );

        VkCommandBufferBeginInfo commandBufferBeginInfo;
        InitializeStruct( commandBufferBeginInfo );
        commandBufferBeginInfo.flags |= VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        CheckedCall( vkBeginCommandBuffer( cmdBuffer, &commandBufferBeginInfo ) );

        VkClearValue clearValue[2];
        clearValue[0].color.float32[ 0 ] = clearColor[ 0 ];
        clearValue[0].color.float32[ 1 ] = clearColor[ 1 ];
        clearValue[0].color.float32[ 2 ] = clearColor[ 2 ];
        clearValue[0].color.float32[ 3 ] = clearColor[ 3 ];
        clearValue[1].depthStencil.depth = 1;
        clearValue[1].depthStencil.stencil = 0;

        VkRenderPassBeginInfo renderPassBeginInfo;
        InitializeStruct( renderPassBeginInfo );
        renderPassBeginInfo.renderPass               = appContent->hDbgRenderPass;
        renderPassBeginInfo.framebuffer              = appContent->hDbgFramebuffers[ appContent->FrameIndex ];
        renderPassBeginInfo.renderArea.extent.width  = appSurfaceVk->GetWidth( );
        renderPassBeginInfo.renderArea.extent.height = appSurfaceVk->GetHeight( );
        renderPassBeginInfo.clearValueCount          = 2;
        renderPassBeginInfo.pClearValues             = clearValue;

        vkCmdBeginRenderPass( cmdBuffer, &renderPassBeginInfo, VK_SUBPASS_CONTENTS_INLINE );

        DebugRendererVk::FrameUniformBuffer frameData;

        static float rotationY = 0.0;

        rotationY += 0.001;
        if (rotationY >= 2 * M_PI) {
            rotationY -= 2 * M_PI;
        }

        frameData.worldMatrix = mathfu::mat4::FromRotationMatrix( mathfu::mat3::RotationY( rotationY ) );
        frameData.projectionMatrix = mathfu::mat4::Perspective( 55.0f / 180.0f * M_PI, (float) width / (float) height, 0.1f, 100.0f, 1 );
        frameData.viewMatrix = mathfu::mat4::LookAt( {0, 0, 0}, {0, 3, 5}, {0, 1, 0}, 1 );
        frameData.color = {1, 0, 0, 1};

        // Flip projection matrix from GL to Vulkan orientation.
        frameData.projectionMatrix.GetColumn( 1 )[ 1 ] *= -1;

        appContent->Dbg->Reset( appContent->FrameIndex );

        DebugRendererVk::RenderParametersVk renderParamsDbg;
        renderParamsDbg.dims[ 0 ]  = (float) width;
        renderParamsDbg.dims[ 1 ]  = (float) height;
        renderParamsDbg.scale[ 0 ] = 1;
        renderParamsDbg.scale[ 1 ] = 1;
        renderParamsDbg.FrameIndex = appContent->FrameIndex;
        renderParamsDbg.pCmdBuffer = cmdBuffer;
        renderParamsDbg.pFrameData = &frameData;

        //appContent->Dbg->Render( &renderParamsDbg );

        const float scale = 0.5f;

        frameData.worldMatrix
            = mathfu::mat4::FromRotationMatrix( mathfu::mat3::RotationY( rotationY ) )
            * mathfu::mat4::FromScaleVector({ scale, scale * 2, scale })
            * mathfu::mat4::FromTranslationVector(mathfu::vec3{ 0, scale * 3, 0 });

        frameData.color = { 0, 1, 0, 1 };
        appContent->Dbg->Render(&renderParamsDbg);

        frameData.worldMatrix
            = mathfu::mat4::FromRotationMatrix( mathfu::mat3::RotationY( rotationY ) )
            * mathfu::mat4::FromScaleVector({ scale, scale, scale * 2 })
            * mathfu::mat4::FromTranslationVector(mathfu::vec3{ 0, 0, scale * 3 });

        frameData.color = { 0, 0, 1, 1 };
        appContent->Dbg->Render(&renderParamsDbg);

        frameData.worldMatrix
            = mathfu::mat4::FromRotationMatrix( mathfu::mat3::RotationY( rotationY ) )
            * mathfu::mat4::FromScaleVector({ scale * 2, scale, scale })
            * mathfu::mat4::FromTranslationVector(mathfu::vec3{ scale * 3, 0, 0 });

        frameData.color = { 1, 0, 0, 1 };
        appContent->Dbg->Render(&renderParamsDbg);

        frameData.worldMatrix
            = mathfu::mat4::FromRotationMatrix(mathfu::mat3::RotationY(rotationY))
            * mathfu::mat4::FromTranslationVector(mathfu::vec3{ 0, 0, 0 })
            * mathfu::mat4::FromScaleVector({ scale / 2, scale / 2, scale / 2 });

        frameData.color = { 0.5, 0.5, 0.5, 1 };
        appContent->Dbg->Render(&renderParamsDbg);

        //vkCmdEndRenderPass( cmdBuffer );

        /*VkRenderPassBeginInfo renderPassBeginInfoNk;
        InitializeStruct( renderPassBeginInfoNk );
        renderPassBeginInfoNk.renderPass               = appContent->hNkRenderPass;
        renderPassBeginInfoNk.framebuffer              = framebufferNk;
        renderPassBeginInfoNk.renderArea.extent.width  = appSurfaceVk->GetWidth( );
        renderPassBeginInfoNk.renderArea.extent.height = appSurfaceVk->GetHeight( );
        renderPassBeginInfoNk.clearValueCount          = 1;
        renderPassBeginInfoNk.pClearValues             = clearValue;

        vkCmdBeginRenderPass( cmdBuffer, &renderPassBeginInfoNk, VK_SUBPASS_CONTENTS_INLINE );*/

        NuklearSdlVk::RenderParametersVk renderParamsNk;
        renderParamsNk.dims[ 0 ]          = (float) width;
        renderParamsNk.dims[ 1 ]          = (float) height;
        renderParamsNk.scale[ 0 ]         = 1;
        renderParamsNk.scale[ 1 ]         = 1;
        renderParamsNk.aa                 = NK_ANTI_ALIASING_ON;
        renderParamsNk.max_vertex_buffer  = 64 * 1024;
        renderParamsNk.max_element_buffer = 64 * 1024;
        renderParamsNk.FrameIndex         = appContent->FrameIndex;
        renderParamsNk.pCmdBuffer         = cmdBuffer;

        appContent->Nk->Render( &renderParamsNk );

        vkCmdEndRenderPass( cmdBuffer );

        VkPipelineStageFlags waitPipelineStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;

        VkSubmitInfo submitInfo;
        InitializeStruct( submitInfo );
        submitInfo.signalSemaphoreCount = 1;
        submitInfo.pSignalSemaphores    = &renderCompleteSemaphore;
        submitInfo.waitSemaphoreCount   = 1;
        submitInfo.pWaitSemaphores      = &presentCompleteSemaphore;
        submitInfo.pWaitDstStageMask    = &waitPipelineStage;
        submitInfo.commandBufferCount   = 1;
        submitInfo.pCommandBuffers      = &cmdBuffer;

        appContent->Dbg->Flush( appContent->FrameIndex );

        CheckedCall( vkEndCommandBuffer( cmdBuffer ) );
        CheckedCall( vkResetFences( device, 1, &fence ) );
        CheckedCall( vkQueueSubmit( queue, 1, &submitInfo, fence ) );

        if ( appContent->FrameId ) {
            uint32_t    presentIndex    = ( appContent->FrameIndex + appContent->FrameCount - 1 ) % appContent->FrameCount;
            VkSemaphore renderSemaphore = appContent->hRenderCompleteSemaphores[ presentIndex ];

            VkPresentInfoKHR presentInfoKHR;
            InitializeStruct( presentInfoKHR );
            presentInfoKHR.waitSemaphoreCount = 1;
            presentInfoKHR.pWaitSemaphores    = &renderSemaphore;
            presentInfoKHR.swapchainCount     = 1;
            presentInfoKHR.pSwapchains        = &swapchain;
            presentInfoKHR.pImageIndices      = &appContent->BackbufferIndices[ presentIndex ];

            CheckedCall( vkQueuePresentKHR( queue, &presentInfoKHR ) );
        }
    }
}

bool App::IsRunning( ) {
    return AppBase::IsRunning( );
}

extern "C" AppBase* CreateApp( ) {
    return new App( );
}