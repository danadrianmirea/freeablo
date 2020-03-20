#include "../cel/celfile.h"
#include "../level/level.h"
#include "atlastexture.h"
#include "nuklear_sdl_gl3.h"
#include "render.h"
#include "vertextypes.h"
#include <SDL.h>
#include <SDL_image.h>
#include <fa_nuklear.h>
#include <faio/fafileobject.h>
#include <iostream>
#include <misc/assert.h>
#include <misc/savePNG.h>
#include <misc/stringops.h>
#include <render/buffer.h>
#include <render/commandqueue.h>
#include <render/pipeline.h>
#include <render/renderinstance.h>
#include <render/texture.h>
#include <render/vertexarrayobject.h>

// clang-format off
#include <misc/disablewarn.h>
#include "../../extern/jo_gif/jo_gif.cpp"
#include "../../extern/RectangleBinPack/SkylineBinPack.h"
#include <misc/enablewarn.h>
// clang-format on

#if defined(WIN32) || defined(_WIN32)
extern "C" {
__declspec(dllexport) unsigned NvOptimusEnablement = 0x00000001;
}
extern "C" {
__declspec(dllexport) unsigned AmdPowerXpressRequestHighPerformance = 0x00000001;
}
#else
extern "C" {
int NvOptimusEnablement = 1;
}
extern "C" {
int AmdPowerXpressRequestHighPerformance = 1;
}
#endif

namespace Render
{
    // atlasTexture is unique_ptr as to not instantiate until opengl is setup.
    std::unique_ptr<AtlasTexture> atlasTexture = nullptr;

    /* Caches level sprites/positions etc in a format that can be directly injected into GL VBOs. */
    class DrawLevelCache
    {
    public:
        DrawLevelCache(size_t initSize) { mInstanceData.resize(initSize); }

        void addSprite(uint32_t sprite, int32_t x, int32_t y, std::optional<Cel::Colour> highlightColor)
        {
            auto& lookupMap = atlasTexture->getLookupMap();
            auto& atlasEntry = lookupMap.at(sprite);

            SpriteVertexPerInstance vertexData = {};

            vertexData.v_imageSize[0] = atlasEntry.mWidth;
            vertexData.v_imageSize[1] = atlasEntry.mHeight;

            vertexData.v_atlasOffset[0] = atlasEntry.mX;
            vertexData.v_atlasOffset[1] = atlasEntry.mY;
            vertexData.v_atlasOffset[2] = atlasEntry.mLayer;

            vertexData.v_imageOffset[0] = x;
            vertexData.v_imageOffset[1] = y;

            if (auto c = highlightColor)
            {
                vertexData.v_hoverColor[0] = c->r;
                vertexData.v_hoverColor[1] = c->g;
                vertexData.v_hoverColor[2] = c->b;
                vertexData.v_hoverColor[3] = 255;
            }

            mInstanceData.push_back(vertexData);
        }

        size_t instanceCount() const { return mInstanceData.size(); }

        std::pair<void*, size_t> getData() { return {mInstanceData.data(), mInstanceData.size() * sizeof(SpriteVertexPerInstance)}; }

        void clear() { mInstanceData.clear(); }

        std::vector<SpriteVertexPerInstance> mInstanceData;
    };

    int32_t WIDTH = 1280;
    int32_t HEIGHT = 960;

    SDL_Window* screen;
    // SDL_Renderer* renderer;
    RenderInstance* renderInstance = nullptr;
    CommandQueue* mainCommandQueue = nullptr;
    VertexArrayObject* vertexArrayObject = nullptr;
    Pipeline* drawLevelPipeline = nullptr;
    Buffer* drawLevelVertexUniformBuffer = nullptr;
    DescriptorSet* drawLevelDescriptorSet = nullptr;

    struct DrawLevelUniforms
    {
        struct Vertex
        {
            float screenSize[2];

            float _pad[2];
        } vertex;

        struct Fragment
        {
            float atlasSize[2];

            float _pad[2];
        } fragment;
    };

    DrawLevelCache drawLevelCache = DrawLevelCache(2000);
    std::string windowTitle;

    void init(const std::string& title, const RenderSettings& settings, NuklearGraphicsContext& nuklearGraphics, nk_context* nk_ctx)
    {
        WIDTH = settings.windowWidth;
        HEIGHT = settings.windowHeight;
        int flags = SDL_WINDOW_OPENGL;
        windowTitle = title;

        if (settings.fullscreen)
        {
            flags |= SDL_WINDOW_FULLSCREEN;
        }
        else
        {
            flags |= SDL_WINDOW_RESIZABLE;
        }

        SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO);
        screen = SDL_CreateWindow(title.c_str(), 20, 20, WIDTH, HEIGHT, flags);
        if (screen == nullptr)
            printf("Could not create window: %s\n", SDL_GetError());

        renderInstance = RenderInstance::createRenderInstance(RenderInstance::Type::OpenGL, *screen);
        mainCommandQueue = renderInstance->createCommandQueue().release();
        mainCommandQueue->begin();

        PipelineSpec drawLevelPipelineSpec;
        drawLevelPipelineSpec.vertexLayouts = {SpriteVertexMain::layout(), SpriteVertexPerInstance::layout()};
        drawLevelPipelineSpec.vertexShaderPath = Misc::getResourcesPath().str() + "/shaders/basic.vert";
        drawLevelPipelineSpec.fragmentShaderPath = Misc::getResourcesPath().str() + "/shaders/basic.frag";
        drawLevelPipelineSpec.descriptorSetSpec = {{
            {DescriptorType::UniformBuffer, "vertexUniforms"},
            {DescriptorType::UniformBuffer, "fragmentUniforms"},
            {DescriptorType::Texture, "tex"},
        }};

        drawLevelPipeline = renderInstance->createPipeline(drawLevelPipelineSpec).release();
        vertexArrayObject = renderInstance->createVertexArrayObject({0, 0}, drawLevelPipelineSpec.vertexLayouts, 0).release();
        drawLevelVertexUniformBuffer = renderInstance->createBuffer(sizeof(DrawLevelUniforms)).release();
        drawLevelDescriptorSet = renderInstance->createDescriptorSet(drawLevelPipelineSpec.descriptorSetSpec).release();
        atlasTexture = std::make_unique<AtlasTexture>(*renderInstance, *mainCommandQueue);

        drawLevelDescriptorSet->updateItems({
            {0, BufferSlice{drawLevelVertexUniformBuffer, offsetof(DrawLevelUniforms, vertex), sizeof(DrawLevelUniforms::Vertex)}},
            {1, BufferSlice{drawLevelVertexUniformBuffer, offsetof(DrawLevelUniforms, fragment), sizeof(DrawLevelUniforms::Fragment)}},
            {2, &atlasTexture->getTextureArray()},
        });

        // clang-format off
        SpriteVertexMain baseVertices[] =
        {
            {{0, 0, 0},  {0, 0}},
            {{1, 0, 0},  {1, 0}},
            {{1, 1, 0},  {1, 1}},
            {{0, 0, 0},  {0, 0}},
            {{1, 1, 0},  {1, 1}},
            {{0, 1, 0},  {0, 1}}
        };
        // clang-format on
        vertexArrayObject->getVertexBuffer(0)->setData(baseVertices, sizeof(baseVertices));

        // Update screen with/height, as starting full screen window in
        // Windows does not trigger a SDL_WINDOWEVENT_RESIZED event.
        SDL_GetWindowSize(screen, &WIDTH, &HEIGHT);

        if (nk_ctx)
        {
            nuklearGraphics = {};
            nk_sdl_device_create(nuklearGraphics.dev, *renderInstance);
        }
    }

    void setWindowSize(const RenderSettings& settings) { SDL_SetWindowSize(screen, settings.windowWidth, settings.windowHeight); }

    const std::string& getWindowTitle() { return windowTitle; }
    void setWindowTitle(const std::string& title) { SDL_SetWindowTitle(screen, title.c_str()); }

    void destroyNuklearGraphicsContext(NuklearGraphicsContext& nuklearGraphics)
    {
        nk_font_atlas_clear(&nuklearGraphics.atlas);
        nk_sdl_device_destroy(nuklearGraphics.dev);
    }

    void quit()
    {
        atlasTexture.reset();
        delete drawLevelDescriptorSet;
        delete drawLevelVertexUniformBuffer;
        delete vertexArrayObject;
        delete drawLevelPipeline;
        delete mainCommandQueue;
        delete renderInstance;
        // SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(screen);
        SDL_Quit();
    }

    bool resized = false;

    void resize(size_t w, size_t h)
    {
        WIDTH = w;
        HEIGHT = h;
        resized = true;

        renderInstance->onWindowResized(WIDTH, HEIGHT);
    }

    RenderSettings getWindowSize()
    {
        RenderSettings settings;
        settings.windowWidth = WIDTH;
        settings.windowHeight = HEIGHT;
        // SDL_GetRendererOutputSize(renderer, &settings.windowWidth, &settings.windowHeight);
        // SDL_GetWindowSize(screen, &settings.windowWidth, &settings.windowHeight);
        return settings;
    }

    uint32_t getGLTexFromSurface(SDL_Surface* surf)
    {
        bool validFormat = true;
        if (surf->format->BitsPerPixel != 24 && surf->format->BitsPerPixel != 32)
            validFormat = false;
        if (surf->format->Rmask != 0x000000FF || surf->format->Gmask != 0x0000FF00 || surf->format->Bmask != 0x00FF0000)
            validFormat = false;
        if (surf->format->BitsPerPixel != 32 || surf->format->Amask != 0xFF000000)
            validFormat = false;
        if (surf->pitch != 4 * surf->w)
            validFormat = false;

        if (!validFormat)
            surf = SDL_ConvertSurfaceFormat(
                surf, SDL_PIXELFORMAT_ABGR8888, 0); // SDL is stupid and interprets pixel formats by endianness, so on LE, it calls RGBA ABGR...

        debug_assert(surf->pitch == 4 * surf->w);

        uint32_t id = atlasTexture->addTexture(surf->w, surf->h, surf->pixels);

        if (!validFormat)
            SDL_FreeSurface(surf);

        return id;
    }

    void drawGui(NuklearFrameDump& dump, SpriteCacheBase* cache) { nk_sdl_render_dump(cache, dump, screen, *atlasTexture, *mainCommandQueue); }

    SDL_Surface* loadNonCelImage(const std::string& sourcePath, const std::string& extension)
    {
        FAIO::FAFileObject file_handle(sourcePath);
        if (!file_handle.isValid())
            return NULL;

        size_t buffer_size = file_handle.FAsize();

        char* buffer = new char[buffer_size];
        file_handle.FAfread(buffer, 1, buffer_size);

        SDL_Surface* s = IMG_LoadTyped_RW(SDL_RWFromMem(buffer, buffer_size), 1, extension.c_str());
        delete[] buffer;

        return s;
    }

    std::string getImageExtension(const std::string& path)
    {
        size_t i;
        for (i = path.length() - 1; i > 0; i--)
        {
            if (path[i] == '.')
                break;
        }

        return path.substr(i + 1, path.length() - i);
    }

    bool getImageInfo(const std::string& path, std::vector<int32_t>& widths, std::vector<int32_t>& heights, int32_t& animLength)
    {
        // TODO: get better image decoders that allow you to peek image dimensions without loading full image

        std::string extension = getImageExtension(path);

        if (Misc::StringUtils::ciEqual(extension, "cel") || Misc::StringUtils::ciEqual(extension, "cl2"))
        {
            Cel::CelFile cel(path);
            widths.resize(cel.animLength());
            heights.resize(cel.animLength());
            for (int i = 0; i < cel.animLength(); ++i)
            {
                widths[i] = cel[i].width();
                heights[i] = cel[i].height();
            }
            animLength = cel.animLength();
        }
        else
        {
            SDL_Surface* surface = loadNonCelImage(path, extension);

            if (surface)
            {
                widths = {surface->w};
                heights = {surface->h};
                animLength = 1;

                SDL_FreeSurface(surface);
            }
            else
            {
                return false;
            }
        }

        return true;
    }

    Cel::Colour getPixel(const SDL_Surface* s, int x, int y);
    void setpixel(SDL_Surface* s, int x, int y, Cel::Colour c);
    SDL_Surface* createTransparentSurface(size_t width, size_t height);
    void drawFrame(SDL_Surface* s, int start_x, int start_y, const Cel::CelFrame& frame);

    SDL_Surface* loadNonCelImageTrans(const std::string& path, const std::string& extension, bool hasTrans, size_t transR, size_t transG, size_t transB)
    {
        SDL_Surface* tmp = loadNonCelImage(path, extension);

        if (hasTrans)
        {
            SDL_Surface* src = tmp;
            tmp = createTransparentSurface(src->w, src->h);

            for (int x = 0; x < src->w; x++)
            {
                for (int y = 0; y < src->h; y++)
                {
                    Cel::Colour px = getPixel(src, x, y);
                    if (!(px.r == transR && px.g == transG && px.b == transB))
                    {
                        setpixel(tmp, x, y, px);
                    }
                }
            }
            SDL_FreeSurface(src);
        }

        return tmp;
    }

    SpriteGroup* loadSprite(const std::string& path, bool hasTrans, size_t transR, size_t transG, size_t transB)
    {
        std::string extension = getImageExtension(path);

        if (Misc::StringUtils::ciEqual(extension, "cel") || Misc::StringUtils::ciEqual(extension, "cl2"))
        {
            return new SpriteGroup(path);
        }
        else
        {
            SDL_Surface* tmp = loadNonCelImageTrans(path, extension, hasTrans, transR, transG, transB);

            std::vector<Sprite> vec(1);
            vec[0] = (Sprite)(intptr_t)getGLTexFromSurface(tmp);

            SDL_FreeSurface(tmp);

            return new SpriteGroup(vec);
        }
    }

    void clearTransparentSurface(SDL_Surface* s);

    SpriteGroup* loadVanimSprite(const std::string& path, size_t vAnim, bool hasTrans, size_t transR, size_t transG, size_t transB)
    {
        std::string extension = getImageExtension(path);
        SDL_Surface* original = loadNonCelImageTrans(path, extension, hasTrans, transR, transG, transB);

        SDL_Surface* tmp = createTransparentSurface(original->w, vAnim);

        std::cout << original->w;

        std::vector<Sprite> vec;

        for (size_t srcY = 0; srcY < (size_t)original->h - 1; srcY += vAnim)
        {
            for (size_t x = 0; x < (size_t)original->w; x++)
            {
                for (size_t y = 0; y < vAnim; y++)
                {
                    if (srcY + y < (size_t)original->h)
                    {
                        Cel::Colour px = getPixel(original, x, srcY + y);
                        setpixel(tmp, x, y, px);
                    }
                }
            }

            vec.push_back((Render::Sprite)(intptr_t)getGLTexFromSurface(tmp));

            clearTransparentSurface(tmp);
        }

        SDL_FreeSurface(original);
        SDL_FreeSurface(tmp);

        return new SpriteGroup(vec);
    }

    SpriteGroup* loadResizedSprite(
        const std::string& path, size_t width, size_t height, size_t tileWidth, size_t tileHeight, bool hasTrans, size_t transR, size_t transG, size_t transB)
    {
        std::string extension = getImageExtension(path);
        SDL_Surface* original = loadNonCelImageTrans(path, extension, hasTrans, transR, transG, transB);
        SDL_Surface* tmp = createTransparentSurface(width, height);

        size_t srcX = 0;
        size_t srcY = 0;
        size_t dstX = 0;
        size_t dstY = 0;

        while (true)
        {
            for (size_t y = 0; y < tileHeight; y += 1)
            {
                for (size_t x = 0; x < tileWidth; x += 1)
                {
                    Cel::Colour px = getPixel(original, srcX + x, srcY + y);
                    setpixel(tmp, dstX + x, dstY + y, px);
                }
            }

            srcX += tileWidth;
            if (srcX >= (size_t)original->w)
            {
                srcX = 0;
                srcY += tileHeight;
            }

            if (srcY >= (size_t)original->h)
                break;

            dstX += tileWidth;
            if (dstX >= width)
            {
                dstX = 0;
                dstY += tileHeight;
            }

            if (dstY >= height)
                break;
        }

        std::vector<Sprite> vec(1);
        vec[0] = (Sprite)(intptr_t)getGLTexFromSurface(tmp);

        SDL_FreeSurface(original);
        SDL_FreeSurface(tmp);

        return new SpriteGroup(vec);
    }

    SpriteGroup* loadCelToSingleTexture(const std::string& path)
    {
        Cel::CelFile cel(path);

        int32_t width = 0;
        int32_t height = 0;

        for (int32_t i = 0; i < cel.numFrames(); i++)
        {
            width += cel[i].width();
            height = (cel[i].height() > height ? cel[i].height() : height);
        }

        debug_assert(width > 0);
        debug_assert(height > 0);

        SDL_Surface* surface = createTransparentSurface(width, height);

        int32_t x = 0;
        for (int32_t i = 0; i < cel.numFrames(); i++)
        {
            drawFrame(surface, x, 0, cel[i]);
            x += cel[i].width();
        }

        std::vector<Sprite> vec(1);
        vec[0] = (Sprite)(intptr_t)getGLTexFromSurface(surface);

        SDL_FreeSurface(surface);

        return new SpriteGroup(vec);
    }

    SpriteGroup* loadTiledTexture(const std::string& sourcePath, size_t width, size_t height, bool hasTrans, size_t transR, size_t transG, size_t transB)
    {
        std::string extension = getImageExtension(sourcePath);
        SDL_Surface* tile = loadNonCelImageTrans(sourcePath, extension, hasTrans, transR, transG, transB);
        SDL_Surface* texture = createTransparentSurface(width, height);

        int dx = tile->w;
        int dy = tile->h;

        for (size_t y = 0; y < height; y += dy)
        {
            for (size_t x = 0; x < width; x += dx)
            {
                for (size_t sy = 0; sy < (size_t)tile->h && (y + sy) < height; sy++)
                {
                    for (size_t sx = 0; sx < (size_t)tile->w && (x + sx) < width; sx++)
                    {
                        Cel::Colour px = getPixel(tile, sx, sy);
                        setpixel(texture, x + sx, y + sy, px);
                    }
                }
            }
        }

        std::vector<Sprite> vec(1);
        vec[0] = (Sprite)(intptr_t)getGLTexFromSurface(texture);

        SDL_FreeSurface(texture);
        SDL_FreeSurface(tile);

        return new SpriteGroup(vec);
    }

    SpriteGroup* loadNonCelSprite(const std::string& path)
    {
        std::string extension = getImageExtension(path);
        SDL_Surface* image = loadNonCelImage(path, extension);

        std::vector<Sprite> vec(1);
        vec[0] = (Sprite)(intptr_t)getGLTexFromSurface(image);

        SDL_FreeSurface(image);

        return new SpriteGroup(vec);
    }

    FACursor createCursor(const Cel::CelFrame& celFrame, int32_t hot_x, int32_t hot_y)
    {
        auto surface = createTransparentSurface(celFrame.width(), celFrame.height());
        drawFrame(surface, 0, 0, celFrame);
        auto cursor = SDL_CreateColorCursor(surface, hot_x, hot_y);
        SDL_FreeSurface(surface);
        return (FACursor)cursor;
    }

    void freeCursor(FACursor cursor) { SDL_FreeCursor((SDL_Cursor*)cursor); }

    void drawCursor(FACursor cursor)
    {
        if (cursor == NULL)
        {
            cursor = (FACursor)SDL_GetDefaultCursor();
        }

        SDL_SetCursor((SDL_Cursor*)cursor);
        SDL_ShowCursor(SDL_ENABLE);
    }

    SpriteGroup* loadSprite(const uint8_t* source, size_t width, size_t height)
    {
#if SDL_BYTEORDER == SDL_BIG_ENDIAN
        Uint32 rmask = 0xff000000;
        Uint32 gmask = 0x00ff0000;
        Uint32 bmask = 0x0000ff00;
        Uint32 amask = 0x000000ff;
#else
        Uint32 rmask = 0x000000ff;
        Uint32 gmask = 0x0000ff00;
        Uint32 bmask = 0x00ff0000;
        Uint32 amask = 0xff000000;
#endif

        SDL_Surface* surface = SDL_CreateRGBSurfaceFrom((void*)source, width, height, 32, width * 4, rmask, gmask, bmask, amask);
        // SDL_Texture *tex = SDL_CreateTextureFromSurface(renderer, surface);
        // SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);

        std::vector<Sprite> vec(1);
        vec[0] = (Sprite)(intptr_t)getGLTexFromSurface(surface);

        SDL_FreeSurface(surface);

        return new SpriteGroup(vec);
    }

    bool once = false;

    void deleteAllSprites() { atlasTexture->clear(*mainCommandQueue); }

    void draw()
    {
        mainCommandQueue->cmdPresent();
        mainCommandQueue->end();
        mainCommandQueue->submit();
        mainCommandQueue->begin();
    }

    void drawSprite(uint32_t sprite, int32_t x, int32_t y, std::optional<Cel::Colour> highlightColor)
    {
        // Add to level cache, will be drawn in a batch later.
        drawLevelCache.addSprite(sprite, x, y, highlightColor);
    }

    static void drawCachedLevel()
    {
        DrawLevelUniforms uniforms = {};
        uniforms.vertex.screenSize[0] = WIDTH;
        uniforms.vertex.screenSize[1] = HEIGHT;
        uniforms.fragment.atlasSize[0] = atlasTexture->getTextureArray().width();
        uniforms.fragment.atlasSize[1] = atlasTexture->getTextureArray().height();
        drawLevelVertexUniformBuffer->setData(&uniforms, sizeof(DrawLevelUniforms));

        std::pair<void*, size_t> instanceData = drawLevelCache.getData();
        vertexArrayObject->getVertexBuffer(1)->setData(instanceData.first, instanceData.second);

        Bindings bindings;
        bindings.vao = vertexArrayObject;
        bindings.pipeline = drawLevelPipeline;
        bindings.descriptorSet = drawLevelDescriptorSet;

        // Draw the whole level in one batched operation.
        mainCommandQueue->cmdDrawInstances(0, 6, drawLevelCache.instanceCount(), bindings);
    }

    void handleEvents()
    {
        SDL_Event event;
        while (SDL_PollEvent(&event))
        {
            // do nothing, just clear the event queue to avoid render window hang up in ubuntu.
        }
    }

    void drawSprite(const Sprite& sprite, int32_t x, int32_t y, std::optional<Cel::Colour> highlightColor)
    {
        drawSprite((uint32_t)(intptr_t)sprite, x, y, highlightColor);
    }

    constexpr auto tileHeight = 32;
    constexpr auto tileWidth = tileHeight * 2;

    void drawAtTile(const Sprite& sprite, const Misc::Point& tileTop, int spriteW, int spriteH, std::optional<Cel::Colour> highlightColor = std::nullopt)
    {
        // centering spright at the center of tile by width and at the bottom of tile by height
        drawSprite(sprite, tileTop.x - spriteW / 2, tileTop.y - spriteH + tileHeight, highlightColor);
    }

    SpriteGroup::SpriteGroup(const std::string& path)
    {
        Cel::CelFile cel(path);

        for (int32_t i = 0; i < cel.numFrames(); i++)
        {
            SDL_Surface* s = createTransparentSurface(cel[i].width(), cel[i].height());
            drawFrame(s, 0, 0, cel[i]);

            mSprites.push_back((Render::Sprite)(intptr_t)getGLTexFromSurface(s)); // SDL_CreateTextureFromSurface(renderer, s));

            SDL_FreeSurface(s);
        }
        mWidth = cel[0].width();
        mHeight = cel[0].height();

        mAnimLength = cel.animLength();
    }

    Sprite& SpriteGroup::operator[](size_t index)
    {
        debug_assert(index < mSprites.size());
        return mSprites[index];
    }

    void SpriteGroup::toPng(const std::string& celPath, const std::string& pngPath)
    {
        Cel::CelFile cel(celPath);

        int32_t numFrames = cel.numFrames();

        if (numFrames == 0)
            return;

        int32_t sumWidth = 0;
        int32_t maxHeight = 0;
        for (int32_t i = 0; i < numFrames; i++)
        {
            sumWidth += cel[i].width();
            if (cel[i].height() > maxHeight)
                maxHeight = cel[i].height();
        }

        if (sumWidth == 0)
            return;

        SDL_Surface* s = createTransparentSurface(sumWidth, maxHeight);
        unsigned int x = 0;
        unsigned int dx = 0;
        for (int32_t i = 0; i < numFrames; i++)
        {
            drawFrame(s, x, 0, cel[i]);
            dx = cel[i].width();
            x += dx;
        }

        SDL_SavePNG(s, pngPath.c_str());

        SDL_FreeSurface(s);
    }

    void SpriteGroup::toGif(const std::string& celPath, const std::string& gifPath)
    {
        Cel::CelFile cel(celPath);

        int32_t width = cel[0].width();
        int32_t height = cel[0].height();

        int32_t numFrames = cel.numFrames();

        if (numFrames == 0)
            return;

        jo_gif_t gif = jo_gif_start(gifPath.c_str(), width, height, 0, 256);

        for (int32_t i = 0; i < numFrames; i++)
        {
            SDL_Surface* s = createTransparentSurface(width, height);
            drawFrame(s, 0, 0, cel[i]);

            uint8_t** gifImage = (uint8_t**)malloc(s->h * sizeof(uint8_t*));

            for (int j = 0; j < s->h; j++)
            {
                gifImage[j] = (uint8_t*)(uint8_t**)s->pixels + j * s->pitch;
            }

            jo_gif_frame(&gif, *gifImage, 10, true, 0x00, 0xFF, 0x00);

            free(gifImage);

            SDL_FreeSurface(s);
        }

        jo_gif_end(&gif);
    }

    bool SpriteGroup::canDeleteIndividualSprites()
    {
        // Sprites can not currently be removed from atlas texture.
        return false;
    }

    void SpriteGroup::destroy()
    {
        // Sprites can not currently be removed from atlas texture.
        release_assert(false);

        // for (size_t i = 0; i < mSprites.size(); i++)
        // {
        //     GLuint tex = (GLuint)(intptr_t)mSprites[i];
        //     glDeleteTextures(1, &tex);
        // }
    }

    void drawMinPillarTop(SDL_Surface* s, int x, int y, const std::vector<int16_t>& pillar, Cel::CelFile& tileset);
    void drawMinPillarBase(SDL_Surface* s, int x, int y, const std::vector<int16_t>& pillar, Cel::CelFile& tileset);

    SpriteGroup* loadTilesetSprite(const std::string& celPath, const std::string& minPath, bool top)
    {
        Cel::CelFile cel(celPath);
        Level::Min min(minPath);

        SDL_Surface* newPillar = createTransparentSurface(64, 256);

        std::vector<Sprite> newMin(min.size() - 1);

        for (size_t i = 0; i < min.size() - 1; i++)
        {
            clearTransparentSurface(newPillar);

            if (top)
                drawMinPillarTop(newPillar, 0, 0, min[i], cel);
            else
                drawMinPillarBase(newPillar, 0, 0, min[i], cel);

            newMin[i] = (Sprite)(intptr_t)getGLTexFromSurface(newPillar); // NULL;// SDL_CreateTextureFromSurface(renderer, newPillar);
        }

        SDL_FreeSurface(newPillar);

        return new SpriteGroup(newMin);
    }

    void spriteSize(const Sprite& sprite, int32_t& w, int32_t& h)
    {
        auto& atlasEntry = atlasTexture->getLookupMap().at((size_t)(intptr_t)sprite);
        w = atlasEntry.mWidth;
        h = atlasEntry.mHeight;
    }

    void clear(int r, int g, int b) { mainCommandQueue->cmdClearCurrentFramebuffer(Color(float(r) / 255.0f, float(g) / 255.0f, float(b) / 255.0f, 1.0f)); }

#define BPP 4
#define DEPTH 32

    void clearTransparentSurface(SDL_Surface* s) { SDL_FillRect(s, NULL, SDL_MapRGBA(s->format, 0, 0, 0, 0)); }

    SDL_Surface* createTransparentSurface(size_t width, size_t height)
    {
        SDL_Surface* s;

// SDL y u do dis
#if SDL_BYTEORDER == SDL_BIG_ENDIAN
        s = SDL_CreateRGBSurface(0, width, height, DEPTH, 0xFF000000, 0x00FF0000, 0x0000FF00, 0x000000FF);
#else
        s = SDL_CreateRGBSurface(0, width, height, DEPTH, 0x000000FF, 0x0000FF00, 0x00FF0000, 0xFF000000);
#endif

        clearTransparentSurface(s);

        return s;
    }

    void setpixel(SDL_Surface* surface, int x, int y, Cel::Colour c)
    {
        Uint32 pixel = SDL_MapRGBA(surface->format, c.r, c.g, c.b, ((int)c.visible) * 255);

        int bpp = surface->format->BytesPerPixel;
        // Here p is the address to the pixel we want to set
        Uint8* p = (Uint8*)surface->pixels + y * surface->pitch + x * bpp;

        switch (bpp)
        {
            case 1:
                *p = pixel;
                break;

            case 2:
                *(Uint16*)p = pixel;
                break;

            case 3:
                if (SDL_BYTEORDER == SDL_BIG_ENDIAN)
                {
                    p[0] = (pixel >> 16) & 0xff;
                    p[1] = (pixel >> 8) & 0xff;
                    p[2] = pixel & 0xff;
                }
                else
                {
                    p[0] = pixel & 0xff;
                    p[1] = (pixel >> 8) & 0xff;
                    p[2] = (pixel >> 16) & 0xff;
                }
                break;

            case 4:
                *(Uint32*)p = pixel;
                break;
        }
    }

    Cel::Colour getPixel(const SDL_Surface* s, int x, int y)
    {
        Uint32 pix;

        int bpp = s->format->BytesPerPixel;
        // Here p is the address to the pixel we want to retrieve
        Uint8* p = (Uint8*)s->pixels + y * s->pitch + x * bpp;

        switch (bpp)
        {
            case 1:
                pix = *p;
                break;

            case 2:
                pix = *(Uint16*)p;
                break;

            case 3:
                if (SDL_BYTEORDER == SDL_BIG_ENDIAN)
                    pix = p[0] << 16 | p[1] << 8 | p[2];
                else
                    pix = p[0] | p[1] << 8 | p[2] << 16;
                break;

            case 4:
                pix = *(Uint32*)p;
                break;

            default:
                pix = 0;
        }

        Uint8 r, g, b, a;
        SDL_GetRGBA(pix, s->format, &r, &g, &b, &a);

        return Cel::Colour(r, g, b, a == 255);
    }

    void drawFrame(SDL_Surface* s, int start_x, int start_y, const Cel::CelFrame& frame)
    {
        for (int32_t x = 0; x < frame.width(); x++)
        {
            for (int32_t y = 0; y < frame.height(); y++)
            {
                auto& c = frame.get(x, y);
                if (c.visible)
                    setpixel(s, start_x + x, start_y + y, c);
            }
        }
    }

    void drawMinTile(SDL_Surface* s, Cel::CelFile& f, int x, int y, int16_t l, int16_t r)
    {
        if (l != -1)
            drawFrame(s, x, y, f[l]);

        if (r != -1)
            drawFrame(s, x + 32, y, f[r]);
    }

    void drawMinPillar(SDL_Surface* s, int x, int y, const std::vector<int16_t>& pillar, Cel::CelFile& tileset, bool top)
    {
        // compensate for maps using 5-row min files
        if (pillar.size() == 10)
            y += 3 * 32;

        size_t i, lim;

        if (top)
        {
            i = 0;
            lim = pillar.size() - 2;
        }
        else
        {
            i = pillar.size() - 2;
            lim = pillar.size();
            y += i * 16;
        }

        // Each iteration draw one row of the min
        for (; i < lim; i += 2)
        {
            int16_t l = (pillar[i] & 0x0FFF) - 1;
            int16_t r = (pillar[i + 1] & 0x0FFF) - 1;

            drawMinTile(s, tileset, x, y, l, r);

            y += 32; // down 32 each row
        }
    }

    void drawMinPillarTop(SDL_Surface* s, int x, int y, const std::vector<int16_t>& pillar, Cel::CelFile& tileset)
    {
        drawMinPillar(s, x, y, pillar, tileset, true);
    }

    void drawMinPillarBase(SDL_Surface* s, int x, int y, const std::vector<int16_t>& pillar, Cel::CelFile& tileset)
    {
        drawMinPillar(s, x, y, pillar, tileset, false);
    }

    // basic transform of isometric grid to normal, (0, 0) tile coordinate maps to (0, 0) pixel coordinates
    // since eventually we're gonna shift coordinates to viewport center, it's better to keep transform itself
    // as simple as possible
    template <typename T> static Vec2i tileTopPoint(Vec2<T> tile)
    {
        return Vec2i(Vec2<T>(T(tileWidth / 2) * (tile.x - tile.y), (tile.y + tile.x) * (tileHeight / 2)));
    }

    // this function simply does the reverse of the above function, could be found by solving linear equation system
    // it obviously uses the fact that ttileWidth = tileHeight * 2
    Tile getTileFromScreenCoords(const Misc::Point& screenPos, const Misc::Point& toScreen)
    {
        auto point = screenPos - toScreen;
        auto x = std::div(2 * point.y + point.x, tileWidth); // division by 64 is pretty fast btw
        auto y = std::div(2 * point.y - point.x, tileWidth);
        return {x.quot, y.quot, x.rem > y.rem ? TileHalf::right : TileHalf::left};
    }

    static void
    drawMovingSprite(const Sprite& sprite, const Vec2Fix& fractionalPos, const Misc::Point& toScreen, std::optional<Cel::Colour> highlightColor = std::nullopt)
    {
        int32_t w, h;
        spriteSize(sprite, w, h);
        Vec2i point = tileTopPoint(fractionalPos);
        Vec2i res = point + toScreen;

        drawAtTile(sprite, Vec2i(res), w, h, highlightColor);
    }

    constexpr auto bottomMenuSize = 144; // TODO: pass it as a variable
    Misc::Point worldToScreenVector(const Vec2Fix& fractionalPos)
    {
        // centering takes in accord bottom menu size to be consistent with original game centering
        Vec2i point = tileTopPoint(fractionalPos);

        return Misc::Point{WIDTH / 2, (HEIGHT - bottomMenuSize) / 2} - point;
    }

    Tile getTileByScreenPos(size_t x, size_t y, const Vec2Fix& fractionalPos)
    {
        Misc::Point toScreen = worldToScreenVector(fractionalPos);
        return getTileFromScreenCoords({static_cast<int32_t>(x), static_cast<int32_t>(y)}, toScreen);
    }

    constexpr auto staticObjectHeight = 256;

    template <typename ProcessTileFunc> void drawObjectsByTiles(const Misc::Point& toScreen, ProcessTileFunc processTile)
    {
        Misc::Point start{-2 * tileWidth, -2 * tileHeight};
        Tile startingTile = getTileFromScreenCoords(start, toScreen);

        Misc::Point startingPoint = tileTopPoint(startingTile.pos) + toScreen;
        auto processLine = [&]() {
            Misc::Point point = startingPoint;
            Tile tile = startingTile;

            while (point.x < WIDTH + tileWidth / 2)
            {
                point.x += tileWidth;
                ++tile.pos.x;
                --tile.pos.y;
                processTile(tile, point);
            }
        };

        // then from top left to top-bottom
        while (startingPoint.y < HEIGHT + staticObjectHeight - tileHeight)
        {
            ++startingTile.pos.y;
            startingPoint.x -= tileWidth / 2;
            startingPoint.y += tileHeight / 2;
            processLine();
            ++startingTile.pos.x;
            startingPoint.x += tileWidth / 2;
            startingPoint.y += tileHeight / 2;
            processLine();
        }
    }

    void drawLevel(const Level::Level& level,
                   size_t minTopsHandle,
                   size_t minBottomsHandle,
                   size_t specialSpritesHandle,
                   const std::map<int32_t, int32_t>& specialSpritesMap,
                   SpriteCacheBase* cache,
                   LevelObjects& objs,
                   LevelObjects& items,
                   const Vec2Fix& fractionalPos)
    {
        auto toScreen = worldToScreenVector(fractionalPos);
        SpriteGroup* minBottoms = cache->get(minBottomsHandle);
        auto isInvalidTile = [&](const Tile& tile) {
            return tile.pos.x < 0 || tile.pos.y < 0 || tile.pos.x >= static_cast<int32_t>(level.width()) || tile.pos.y >= static_cast<int32_t>(level.height());
        };

        // drawing on the ground objects
        drawObjectsByTiles(toScreen, [&](const Tile& tile, const Misc::Point& topLeft) {
            if (isInvalidTile(tile))
            {
                // For some reason this code stopped working so for now out of map tiles should be black
                drawAtTile((*minBottoms)[0], topLeft, tileWidth, staticObjectHeight);
                return;
            }

            size_t index = level.get(tile.pos).index();
            if (index < minBottoms->size())
                drawAtTile((*minBottoms)[index], topLeft, tileWidth, staticObjectHeight); // all static objects have the same sprite size
        });

        SpriteGroup* minTops = cache->get(minTopsHandle);
        cache->setImmortal(minTopsHandle, true);

        // drawing above the ground and moving object
        drawObjectsByTiles(toScreen, [&](const Tile& tile, const Misc::Point& topLeft) {
            if (isInvalidTile(tile))
                return;

            size_t index = level.get(tile.pos).index();
            if (index < minTops->size())
            {
                drawAtTile((*minTops)[index], topLeft, tileWidth, staticObjectHeight);

                // Add special sprites (arches / open door frames) if required.
                if (specialSpritesMap.count(index))
                {
                    int32_t specialSpriteIndex = specialSpritesMap.at(index);
                    SpriteGroup* specialSpriteGroup = cache->get(specialSpritesHandle);
                    Sprite& sprite = (*specialSpriteGroup)[specialSpriteIndex];
                    int w, h;
                    spriteSize(sprite, w, h);
                    drawAtTile(sprite, topLeft, w, h);
                }
            }

            auto& itemsForTile = items.get(tile.pos.x, tile.pos.y);
            for (auto& item : itemsForTile)
            {
                int32_t w, h;
                auto sprite = (*cache->get(item.spriteCacheIndex))[item.spriteFrame];
                spriteSize(sprite, w, h);
                drawAtTile(sprite, topLeft, w, h, item.hoverColor);
            }

            auto& objsForTile = objs.get(tile.pos.x, tile.pos.y);
            for (auto& obj : objsForTile)
            {
                if (obj.valid)
                {
                    auto sprite = cache->get(obj.spriteCacheIndex);
                    drawMovingSprite((*sprite)[obj.spriteFrame], obj.fractionalPos, toScreen, obj.hoverColor);
                }
            }
        });

        cache->setImmortal(minTopsHandle, false);

        drawCachedLevel();

        // Clear cached level data after drawing.
        drawLevelCache.clear();

        //        GL_CHECK_ERROR();

#ifdef DEBUG_ATLAS_TEXTURE
        static size_t loop = 0;
        if ((loop++ % 1000) == 0)
            printf("Atlas texture occupancy %.1f%%\n", atlasTexture->getOccupancy());
#endif
    }
}
