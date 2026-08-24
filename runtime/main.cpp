// gfl-psvita runtime: plays IR scene JSON (investigation §4 schema) with SDL2.
// Portable: builds for desktop and PS Vita from this single file.
#include <SDL.h>
#include <SDL_image.h>
#include <SDL_mixer.h>
#include <SDL_ttf.h>

#ifdef GFLVN_VITA
#include <psp2/apputil.h>
#include <psp2/common_dialog.h>
#include <psp2/ime_dialog.h>
#include <psp2/io/stat.h>
#endif

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <sstream>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

using json = nlohmann::json;
namespace fs = std::filesystem;

static const int SCREEN_W = 960;
static const int SCREEN_H = 544;

static bool isFrontFingerDown(const SDL_Event& event) {
    if (event.type != SDL_FINGERDOWN) return false;
#ifdef GFLVN_VITA
    // SDL Vita assigns touch ID 1 to the front panel and ID 2 to the rear.
    // Keep this check even though setup disables rear polling as a second
    // line of defence against accidental story/menu input.
    return event.tfinger.touchId == 1;
#else
    return true;
#endif
}

#define CHECK(cond, msg)                                          \
    do {                                                          \
        if (!(cond)) {                                            \
            std::cerr << "FATAL: " << msg << ": " << SDL_GetError() << "\n"; \
            exit(1);                                              \
        }                                                         \
    } while (0)

struct Tex {
    SDL_Texture* tex = nullptr;
    int w = 0, h = 0;
};

struct PackEntry {
    uint64_t offset = 0;
    uint64_t size = 0;
};

struct PackSlice {
    SDL_RWops* source = nullptr;
    uint64_t offset = 0;
    uint64_t size = 0;
    uint64_t pos = 0;
};

static Sint64 packSize(SDL_RWops* rw) {
    return (Sint64)static_cast<PackSlice*>(rw->hidden.unknown.data1)->size;
}

static Sint64 packSeek(SDL_RWops* rw, Sint64 offset, int whence) {
    auto* slice = static_cast<PackSlice*>(rw->hidden.unknown.data1);
    Sint64 base = whence == RW_SEEK_SET ? 0 :
                  (whence == RW_SEEK_CUR ? (Sint64)slice->pos : (Sint64)slice->size);
    Sint64 target = base + offset;
    if (target < 0 || target > (Sint64)slice->size) return -1;
    if (SDL_RWseek(slice->source, (Sint64)(slice->offset + (uint64_t)target), RW_SEEK_SET) < 0)
        return -1;
    slice->pos = (uint64_t)target;
    return target;
}

static size_t packRead(SDL_RWops* rw, void* ptr, size_t size, size_t maxnum) {
    auto* slice = static_cast<PackSlice*>(rw->hidden.unknown.data1);
    if (size == 0 || maxnum == 0 || slice->pos >= slice->size) return 0;
    uint64_t remaining = slice->size - slice->pos;
    size_t count = (size_t)std::min<uint64_t>(maxnum, remaining / size);
    size_t got = SDL_RWread(slice->source, ptr, size, count);
    slice->pos += (uint64_t)got * size;
    return got;
}

static size_t packWrite(SDL_RWops*, const void*, size_t, size_t) { return 0; }

static int packClose(SDL_RWops* rw) {
    auto* slice = static_cast<PackSlice*>(rw->hidden.unknown.data1);
    int rc = slice->source ? SDL_RWclose(slice->source) : 0;
    delete slice;
    SDL_FreeRW(rw);
    return rc;
}

struct Sprite {
    std::string name;   // char key
    std::string texKey;
    float x = 0, fromX = 0; // gfStory-en centers sprites in equal-width slots
    Uint32 transitionAt = 0;
    bool entering = false;
    bool leaving = false;
    Uint32 leaveAt = 0;
    bool remote = false;
    bool stealth = false;
};

class Player {
public:
    Player(const std::string& rootDir) : root(rootDir) {
        manifest = loadJson(manifestPath());
        fontPath = findFont();
        cjkFontPath = findCjkFont();
    }

    bool loadScene(const std::string& path) {
        scene = loadJson(path);
        currentSceneId = scene.value("id", "");
        if (!autoAdvance && !currentSceneId.empty()) {
            startedScenes.insert(currentSceneId);
            saveProgress();
        }
        // reset per-scene state
        if (audioReady) {
            stopMusic();
            Mix_HaltChannel(-1);
            for (auto* c : liveSfx) Mix_FreeChunk(c);
        }
        liveSfx.clear();
        stage.clear(); nightOn = false; memoryMaskOn = false; blackAlpha = 0; bgId.clear();
        snowOn = false; sparksOn = false; flamesOn = false; eyeMaskHeight = 0;
        for (auto& [_, tex] : texCache) if (tex.tex) SDL_DestroyTexture(tex.tex);
        texCache.clear();
        history.clear(); showLog = false; showScript = false;
        historyScroll = scriptScroll = 0; choiceEv = nullptr;
        callSessionActive = false; callNonRemoteLines = 0; callTransitionAt = 0;
        return true;
    }

    void setup();
    bool isAuto() const { return autoAdvance; }
    std::vector<std::string> sceneFileNames() const {
        std::vector<std::string> files;
        for (const auto& id : manifest["scenes"])
            files.push_back(id.get<std::string>() + ".ir.json");
        return files;
    }
    std::string posLabel;       // "Chapter 1-2 · Part 1" shown in-game
    int run();                       // 0 scene finished, -1 quit, -2 back to menu
    int pickScene(const std::vector<std::string>& names, const std::string& title = "",
                  const std::vector<std::vector<std::string>>& descendants = {},
                  const std::vector<std::string>& logos = {},
                  const std::vector<std::string>& wallpapers = {},
                  const std::string& fixedLogo = "", const std::string& fixedWallpaper = "",
                  bool allowBack = true);

private:
    std::string root;
    std::string packPath;
    std::map<std::string, PackEntry> packEntries;
    std::string bgId;
    json manifest, scene;
    std::string fontPath, cjkFontPath;

    SDL_Window* win = nullptr;
    SDL_Renderer* ren = nullptr;
    TTF_Font* font = nullptr;
    TTF_Font* nameFont = nullptr;
    TTF_Font* uiFont = nullptr;
    TTF_Font* cjkFont = nullptr;
    TTF_Font* cjkNameFont = nullptr;
    TTF_Font* cjkUiFont = nullptr;
    std::map<int, TTF_Font*> scaledFonts;

    std::map<std::string, Tex> texCache;
    std::vector<Sprite> stage;
    bool nightOn = false;
    bool memoryMaskOn = false;
    bool snowOn = false;
    bool sparksOn = false;
    bool flamesOn = false;
    int eyeMaskHeight = 0;
    bool autoAdvance = false;   // GFLVN_AUTO env (CI smoke test)
    bool shotDone = false;      // GFLVN_SHOT debug screenshot
    bool autoMode = false;      // Auto button / R trigger
    int autoSpeed = 1;          // gfStory-en slider: 1x..10x
    bool showLog = false;
    bool showScript = false;
    int historyScroll = 0;
    int scriptScroll = 0;
    Uint8 flashAlpha = 0;       // white flash overlay
    Uint32 shakeUntil = 0;      // screen shake window
    Uint32 autoPageAt = 0;      // when current text page was shown (auto timing)
    int visibleChars = 0;
    bool textAnimating = false;
    const json* choiceEv = nullptr;
    int choiceIndex = 0;
    std::vector<std::pair<std::string, std::string>> history;  // name, line
    Uint8 blackAlpha = 0;   // persistent black overlay (black_on)
    Mix_Music* curMusic = nullptr;
    // SDL_mixer streams Mix_Music from its RWops.  Keep the active packed OGG
    // in RAM so PNG reads from data.gfpak cannot starve the Vita audio thread.
    std::vector<uint8_t> musicBuffer;
    std::vector<Mix_Chunk*> liveSfx;
    bool audioReady = false;
    bool callSessionActive = false;
    int callNonRemoteLines = 0;
    Uint32 callTransitionAt = 0;
    std::set<std::string> startedScenes;
    std::set<std::string> readScenes;
    std::string progressPath;
    std::string currentSceneId;

    void loadPackIndex() {
        packPath = root + "/data.gfpak";
        std::ifstream file(root + "/pack_index.json");
        if (!file) return;  // loose desktop assets remain supported
        json index;
        file >> index;
        for (auto& [name, value] : index["files"].items()) {
            packEntries[name] = {value[0].get<uint64_t>(), value[1].get<uint64_t>()};
        }
        std::cerr << "pack index: " << packEntries.size() << " files\n";
    }

    std::string packKey(const std::string& p) const {
        std::string key = p;
        const std::string rooted = root + "/";
        if (key.rfind(rooted, 0) == 0) key = key.substr(rooted.size());
        else if (key.rfind("assets/", 0) == 0) key = key.substr(7);
        else if (key.rfind("app0:/", 0) == 0) key = key.substr(6);
        return key;
    }

    SDL_RWops* openPacked(const std::string& key) const {
        auto it = packEntries.find(key);
        if (it == packEntries.end()) return nullptr;
        SDL_RWops* source = SDL_RWFromFile(packPath.c_str(), "rb");
        if (!source) return nullptr;
        auto* slice = new PackSlice{source, it->second.offset, it->second.size, 0};
        if (SDL_RWseek(source, (Sint64)slice->offset, RW_SEEK_SET) < 0) {
            SDL_RWclose(source); delete slice; return nullptr;
        }
        SDL_RWops* rw = SDL_AllocRW();
        if (!rw) { SDL_RWclose(source); delete slice; return nullptr; }
        rw->size = packSize;
        rw->seek = packSeek;
        rw->read = packRead;
        rw->write = packWrite;
        rw->close = packClose;
        rw->type = SDL_RWOPS_UNKNOWN;
        rw->hidden.unknown.data1 = slice;
        return rw;
    }

    json loadJson(const std::string& p) {
        const std::string key = packKey(p);
        auto packed = packEntries.find(key);
        if (packed != packEntries.end()) {
            SDL_RWops* rw = openPacked(key);
            if (!rw) { std::cerr << "cannot open packed " << key << "\n"; exit(1); }
            std::string data((size_t)packed->second.size, '\0');
            size_t got = SDL_RWread(rw, data.data(), 1, data.size());
            SDL_RWclose(rw);
            if (got != data.size()) { std::cerr << "short read " << key << "\n"; exit(1); }
            return json::parse(data);
        }
        std::ifstream f(p);
        if (!f) { std::cerr << "cannot open " << p << "\n"; exit(1); }
        json j; f >> j; return j;
    }

    std::string findFont() {
#ifdef GFLVN_VITA
        // app0: is a Vita device path, not a host filesystem path. Probing it
        // through std::filesystem::exists crashes current Vita3K builds.
        return "app0:/fonts/NotoSans-Regular.ttf";
#else
        const std::string candidates[] = {
            root + "/../runtime/fonts/NotoSans-Regular.ttf",
            "runtime/fonts/NotoSans-Regular.ttf",
            "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        };
        for (const auto& p : candidates)
            if (fs::exists(p)) return p;
        return "NotoSans-Regular.ttf";
#endif
    }

    std::string findCjkFont() {
#ifdef GFLVN_VITA
        return "app0:/fonts/NotoSansHans-Regular.ttf";
#else
        const std::string candidates[] = {
            root + "/../runtime/fonts/NotoSansHans-Regular.ttf",
            "runtime/fonts/NotoSansHans-Regular.ttf",
        };
        for (const auto& p : candidates) if (fs::exists(p)) return p;
        return fontPath;
#endif
    }

    std::string manifestPath() {
        const std::string cands[] = { root + "/manifest.json", "assets/manifest.json",
                                      "manifest.json", "app0:/manifest.json" };
        for (const auto& c : cands)
            if (fs::exists(c)) return c;
        return root + "/manifest.json";
    }

    std::string assetPath(const std::string& id) {
        // manifest.img / manifest.aud map key -> path relative to repo root
        std::string rel;
        if (manifest["img"].contains(id)) rel = manifest["img"][id].get<std::string>();
        else if (manifest["aud"].contains(id)) rel = manifest["aud"][id].get<std::string>();
        else return "";
        const std::string packed = packKey(rel);
        if (packEntries.count(packed)) return "@pack:" + packed;
        // candidate layouts: repo-root cwd, root being a subdir, vpk-style root
        const std::string candidates[] = {
            rel,
            root + "/../" + rel,
            root + rel.substr(std::min(rel.size(), size_t(7))),  // strip "assets/"
            root + "/" + rel,
        };
        for (const auto& p : candidates)
            if (fs::exists(p)) return p;
        return rel;
    }

    Tex* getTex(const std::string& id);
    void layoutStage();
    void syncStage(const json& entries);
    void drawAll(int sayPage, const json* sayEv);
    void stopMusic();
    void playMusic(const std::string& id);
    void playSfx(const std::string& id);
    std::vector<std::string> wrapText(const std::string& text, int maxW, TTF_Font* face = nullptr);
    void drawToolbar();
    void drawHistory();
    void drawScript();
    void drawChoices();
    void captureShot();
    void drawDialog(const std::string& name, const std::string& text, int visibleChars, bool animating);
    void drawText(const std::string& text, TTF_Font* face, SDL_Color color, int x, int y);
    void drawTextPrefix(const std::string& text, int visible, TTF_Font* face,
                        SDL_Color color, int x, int y);
    int utf8Length(const std::string& text) const;
    std::string utf8Prefix(const std::string& text, int chars) const;
    std::string displayText(const std::string& text) const;
    SDL_Color markupColor(const std::string& text) const;
    TTF_Font* markupFont(const std::string& text);
    bool containsCjk(const std::string& text) const;
    int toolbarHit(int mx, int my);   // 0..3 button index, else -1
    void loadProgress();
    void saveProgress();
    int readStatus(const std::vector<std::string>& scenes) const;
    void toggleRead(const std::vector<std::string>& scenes);
};

Tex* Player::getTex(const std::string& id) {
    auto it = texCache.find(id);
    if (it != texCache.end()) return &it->second;
    std::string path = assetPath(id);
    if (path.empty()) {
        std::cerr << "WARN: no asset for " << id << "\n";
        texCache[id] = {}; return &texCache[id];
    }
    SDL_Surface* surf = nullptr;
    if (path.rfind("@pack:", 0) == 0) {
        SDL_RWops* rw = openPacked(path.substr(6));
        if (rw) surf = IMG_Load_RW(rw, 1);
    } else {
        surf = IMG_Load(path.c_str());
    }
    if (!surf) { std::cerr << "WARN: IMG_Load " << path << ": " << IMG_GetError() << "\n"; texCache[id] = {}; return &texCache[id]; }
    Tex t;
    t.tex = SDL_CreateTextureFromSurface(ren, surf);
    t.w = surf->w; t.h = surf->h;
    SDL_FreeSurface(surf);
    texCache[id] = t;
    return &texCache[id];
}

// gfStory-en: sprite centers are (i + 1) * width / (count + 1).
void Player::layoutStage() {
    int n = (int)std::count_if(stage.begin(), stage.end(), [](const Sprite& s) { return !s.leaving; });
    int i = 0;
    for (auto& sprite : stage) {
        if (sprite.leaving) continue;
        float target = (++i) * SCREEN_W / (n + 1.0f);
        if (sprite.transitionAt == 0) {
            sprite.fromX = target;
            sprite.x = target;
            sprite.transitionAt = SDL_GetTicks();
        } else if (std::abs(sprite.x - target) > 0.5f) {
            sprite.fromX = sprite.x;
            sprite.x = target;
            sprite.transitionAt = SDL_GetTicks();
        }
    }
}

void Player::syncStage(const json& entries) {
    std::vector<Sprite> next;
    for (const auto& entry : entries) {
        Sprite wanted;
        wanted.name = entry.value("char", "");
        wanted.texKey = "spr_" + wanted.name + "_" + std::to_string(entry.value("expr", 0));
        wanted.remote = entry.value("remote", false);
        wanted.stealth = entry.value("effect", "") == "stealth";
        if (assetPath(wanted.texKey).empty()) continue;
        auto old = std::find_if(stage.begin(), stage.end(), [&](const Sprite& sprite) {
            return !sprite.leaving && sprite.name == wanted.name && sprite.texKey == wanted.texKey;
        });
        // Vue keys sprites by character id, not by expression/texture.  An
        // expression change for the same character updates the image in
        // place; treating it as remove+insert caused an ugly double fade.
        if (old == stage.end()) {
            old = std::find_if(stage.begin(), stage.end(), [&](const Sprite& sprite) {
                return !sprite.leaving && sprite.name == wanted.name;
            });
        }
        if (old != stage.end()) {
            wanted.x = old->x;
            wanted.fromX = old->fromX;
            wanted.transitionAt = old->transitionAt;
            wanted.entering = old->texKey == wanted.texKey && old->entering;
        } else {
            wanted.entering = true;
        }
        next.push_back(wanted);
    }
    // Vue's transition-group keeps removed/replaced sprites for its 200 ms
    // leave transition.  Retaining them is especially visible when a remote
    // call starts or ends.
    Uint32 now = SDL_GetTicks();
    for (auto& old : stage) {
        auto kept = std::find_if(next.begin(), next.end(), [&](const Sprite& sprite) {
            return sprite.name == old.name;
        });
        if (kept == next.end()) {
            if (!old.leaving) {
                old.leaving = true;
                old.leaveAt = now;
                next.push_back(old);
            } else if (now - old.leaveAt < 200) {
                // Do not restart the leave timer at every rapidly-advanced
                // line; that caused replaced sprites to jump back on screen.
                next.push_back(old);
            }
        }
    }
    stage = std::move(next);
    layoutStage();

    bool hasRemote = std::any_of(stage.begin(), stage.end(), [](const Sprite& sprite) {
        return !sprite.leaving && sprite.remote;
    });
    if (hasRemote) {
        callNonRemoteLines = 0;
        if (!callSessionActive) {
            callSessionActive = true;
            callTransitionAt = SDL_GetTicks();
            playSfx("se_AVG_tele_connect");
        }
    } else if (callSessionActive && ++callNonRemoteLines >= 2) {
        callSessionActive = false;
        callNonRemoteLines = 0;
        playSfx("se_AVG_tele_disconnect");
    }
}

void Player::playSfx(const std::string& id) {
    if (!audioReady) return;
    std::string path = assetPath(id);
    if (path.empty()) return;
    Mix_Chunk* chunk = nullptr;
    if (path.rfind("@pack:", 0) == 0) {
        SDL_RWops* rw = openPacked(path.substr(6));
        if (rw) chunk = Mix_LoadWAV_RW(rw, 1);
    } else {
        chunk = Mix_LoadWAV(path.c_str());
    }
    if (!chunk) { std::cerr << "WARN sfx " << path << ": " << Mix_GetError() << "\n"; return; }
    Mix_VolumeChunk(chunk, MIX_MAX_VOLUME / 2);
    Mix_PlayChannel(-1, chunk, 0);
    liveSfx.push_back(chunk);
}

void Player::stopMusic() {
    if (!audioReady) return;
    Mix_HaltMusic();
    if (curMusic) {
        Mix_FreeMusic(curMusic);
        curMusic = nullptr;
    }
    // Mix_FreeMusic closes the memory RWops before its backing store goes away.
    musicBuffer.clear();
}

void Player::playMusic(const std::string& id) {
    if (!audioReady) return;
    if (id == "bgm_BGM_Pause" || id == "bgm_BGM_PAUSE") {
        Mix_PauseMusic();
        return;
    }
    if (id == "bgm_BGM_UnPause" || id == "bgm_BGM_UNPAUSE") {
        Mix_ResumeMusic();
        return;
    }
    std::string path = assetPath(id);
    if (path.empty()) return;
    stopMusic();
    if (path.rfind("@pack:", 0) == 0) {
        const std::string key = path.substr(6);
        auto entry = packEntries.find(key);
        SDL_RWops* packed = openPacked(key);
        if (packed && entry != packEntries.end() && entry->second.size <= 128 * 1024 * 1024ULL) {
            musicBuffer.resize((size_t)entry->second.size);
            const size_t got = SDL_RWread(packed, musicBuffer.data(), 1, musicBuffer.size());
            SDL_RWclose(packed);
            if (got == musicBuffer.size() && !musicBuffer.empty()) {
                SDL_RWops* memory = SDL_RWFromConstMem(musicBuffer.data(), (int)musicBuffer.size());
                if (memory) curMusic = Mix_LoadMUS_RW(memory, 1);
            } else {
                musicBuffer.clear();
            }
        } else if (packed) {
            SDL_RWclose(packed);
        }
    } else {
        curMusic = Mix_LoadMUS(path.c_str());
    }
    if (curMusic) {
        Mix_VolumeMusic(MIX_MAX_VOLUME / 2); // gfStory-en uses volume = 0.5
        Mix_PlayMusic(curMusic, -1);
    }
    else std::cerr << "WARN: music " << path << ": " << Mix_GetError() << "\n";
}

void Player::drawAll(int sayPage, const json* sayEv) {
    // screen shake offset
    int ox = 0, oy = 0;
    if (shakeUntil > SDL_GetTicks()) {
        ox = rand() % 13 - 6; oy = rand() % 9 - 5;
    }
    // background
    SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
    SDL_RenderClear(ren);
    if (!bgId.empty()) {
        Tex* bg = getTex(bgId);
        if (bg->tex) {
            double s = std::max((double)SCREEN_W / bg->w, (double)SCREEN_H / bg->h);
            int w = (int)(bg->w * s), h = (int)(bg->h * s);
            SDL_Rect r{ (SCREEN_W - w) / 2 + ox, (SCREEN_H - h) / 2 + oy, w, h };
            if (memoryMaskOn) SDL_SetTextureColorMod(bg->tex, 196, 173, 126);
            else if (nightOn) SDL_SetTextureColorMod(bg->tex, 112, 112, 255);
            SDL_RenderCopy(ren, bg->tex, nullptr, &r);
            SDL_SetTextureColorMod(bg->tex, 255, 255, 255);
        }
    }
    // Sprites match gfStory-en's 100vh height, 70%-of-screen vertical center,
    // and 200 ms ease transition with a 20 px entering offset.
    for (auto& sp : stage) {
        Tex* t = getTex(sp.texKey);
        if (!t->tex) continue;
        Uint32 now = SDL_GetTicks();
        float elapsed = std::min(1.0f, (now - sp.transitionAt) / 200.0f);
        float eased = 1.0f - (1.0f - elapsed) * (1.0f - elapsed);
        float leaveElapsed = sp.leaving ? std::min(1.0f, (now - sp.leaveAt) / 200.0f) : 0.0f;
        float leaveEased = 1.0f - (1.0f - leaveElapsed) * (1.0f - leaveElapsed);
        float center = sp.fromX + (sp.x - sp.fromX) * eased;
        float scale = (float)SCREEN_H / t->h;
        SDL_Rect r{ 0, 0, (int)(t->w * scale), SCREEN_H };
        r.x = (int)(center - r.w / 2.0f
                    + (sp.entering ? -20.0f * (1.0f - eased) : 0)
                    - (sp.leaving ? 20.0f * leaveEased : 0)) + ox;
        r.y = (int)(SCREEN_H * 0.70f - r.h / 2.0f) + oy;
        float baseAlpha = sp.stealth ? 0.60f : 1.0f;
        float transitionAlpha = (sp.entering ? eased : 1.0f) * (sp.leaving ? 1.0f - leaveEased : 1.0f);
        SDL_SetTextureAlphaMod(t->tex, (Uint8)(255 * baseAlpha * transitionAlpha));
        if (sp.stealth) SDL_SetTextureColorMod(t->tex, 72, 119, 255);
        else if (memoryMaskOn) SDL_SetTextureColorMod(t->tex, 196, 173, 126);
        else if (nightOn) SDL_SetTextureColorMod(t->tex, 112, 112, 255);
        if (!sp.remote) {
            SDL_RenderCopy(ren, t->tex, nullptr, &r);
        } else {
            // SpriteImage.vue framed mode at 960x544:
            // box=0.6h x (11/16), y=0.1h; image gets another 0.1h top pad.
            const int frameH = (int)(SCREEN_H * 0.60f);
            const int frameW = (int)(frameH * 11.0f / 16.0f);
            SDL_Rect frame{ (int)center - frameW / 2 + ox, (int)(SCREEN_H * 0.10f) + oy,
                            frameW, frameH };
            r.x = frame.x + frameW / 2 - r.w / 2;
            r.y = frame.y + (int)(SCREEN_H * 0.10f);

            // Reuse gfStory-en's box-layer.svg itself. Its CSS border box is
            // 18.15 px left / 5.75 px up and 225.15 x 377.65 at Vita size.
            Tex* callFrame = getTex("ui_call_frame");
            if (callFrame->tex) {
                SDL_SetTextureAlphaMod(callFrame->tex, (Uint8)(255 * transitionAlpha));
                SDL_Rect frameLayer{frame.x - 18, frame.y - 6, 248, 355};
                SDL_RenderCopy(ren, callFrame->tex, nullptr, &frameLayer);
                SDL_SetTextureAlphaMod(callFrame->tex, 255);
            }

            SDL_RenderSetClipRect(ren, &frame);
            Uint32 callAge = callTransitionAt ? now - callTransitionAt : 1000;
            const Uint32 callPhase = callAge % 960;
            const bool initialLock = callAge < 360;
            const bool periodicTear = !initialLock && callPhase < 96;
            if (initialLock || periodicTear) {
                // The initial transmission lock tears strongly. While the
                // call remains active a shorter, subtler tear repeats so the
                // framed feed never becomes a completely static portrait.
                const int bands = 12;
                for (int band = 0; band < bands; ++band) {
                    int y1 = frame.y + band * frame.h / bands;
                    int y2 = frame.y + (band + 1) * frame.h / bands;
                    int clippedY1 = std::max(y1, r.y);
                    int clippedY2 = std::min(y2, r.y + r.h);
                    if (clippedY2 <= clippedY1) continue;
                    SDL_Rect src{0,
                        (clippedY1 - r.y) * t->h / r.h,
                        t->w,
                        std::max(1, (clippedY2 - clippedY1) * t->h / r.h)};
                    int strength = initialLock
                        ? std::max(1, (int)((360 - callAge) / 55))
                        : (callPhase < 32 ? 2 : 1);
                    int shift = (((band * 17 + (int)(callAge / 16) * 11) % 9) - 4) * strength;
                    SDL_Rect dst{r.x + shift, clippedY1, r.w, clippedY2 - clippedY1};
                    SDL_RenderCopy(ren, t->tex, &src, &dst);
                }
            } else {
                SDL_RenderCopy(ren, t->tex, nullptr, &r);
            }
            SDL_RenderSetClipRect(ren, nullptr);

            // CSS frame-foreground: cyan/grey 3px radial grid and 16px marker.
            SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
            SDL_SetRenderDrawColor(ren, 0, 255, 255, (Uint8)(28 * transitionAlpha));
            SDL_RenderFillRect(ren, &frame);
            SDL_SetRenderDrawColor(ren, 204, 204, 204, (Uint8)(70 * transitionAlpha));
            for (int fy = frame.y + 1; fy < frame.y + frame.h; fy += 3)
                for (int fx = frame.x + 1; fx < frame.x + frame.w; fx += 3)
                    SDL_RenderDrawPoint(ren, fx, fy);
            SDL_SetRenderDrawColor(ren, 253, 179, 0, (Uint8)(255 * transitionAlpha));
            SDL_Rect marker{ frame.x + 1, frame.y + 1, 16, 16 };
            SDL_RenderFillRect(ren, &marker);
            // A rolling cyan scan line and intermittent transmission tears
            // continue for the entire remote-call session.
            int scanY = frame.y + (int)((callAge / 5) % frame.h);
            SDL_SetRenderDrawColor(ren, 0, 255, 255, (Uint8)(58 * transitionAlpha));
            SDL_Rect scan{frame.x, scanY, frame.w, 2};
            SDL_RenderFillRect(ren, &scan);
            if (initialLock || periodicTear) {
                Uint8 glitchAlpha = initialLock
                    ? (Uint8)(150 * (360 - callAge) / 360.0f * transitionAlpha)
                    : (Uint8)(68 * (96 - callPhase) / 96.0f * transitionAlpha);
                for (int line = 0; line < 7; ++line) {
                    int gy = frame.y + ((line * 47 + callAge / 5) % frame.h);
                    int inset = (line * 29 + callAge / 9) % 42;
                    SDL_SetRenderDrawColor(ren, line & 1 ? 255 : 0, 255, 255, glitchAlpha);
                    SDL_Rect tear{frame.x + inset, gy, std::max(8, frame.w - inset * 2), line % 3 == 0 ? 2 : 1};
                    SDL_RenderFillRect(ren, &tear);
                }
            }
        }
        SDL_SetTextureAlphaMod(t->tex, 255);
        SDL_SetTextureColorMod(t->tex, 255, 255, 255);
        if (elapsed >= 1.0f) sp.entering = false;
    }
    stage.erase(std::remove_if(stage.begin(), stage.end(), [](const Sprite& sp) {
        return sp.leaving && SDL_GetTicks() - sp.leaveAt >= 200;
    }), stage.end());
    // Persistent presentation overlays from the AVG controller. They remain
    // animated even after the typewriter has finished (see the event loop).
    Uint32 overlayNow = SDL_GetTicks();
    SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
    if (memoryMaskOn) {
        SDL_SetRenderDrawColor(ren, 110, 69, 24, 34);
        SDL_RenderFillRect(ren, nullptr);
        for (int inset = 0; inset < 42; inset += 6) {
            SDL_SetRenderDrawColor(ren, 0, 0, 0, (Uint8)(18 - inset / 4));
            SDL_Rect border{inset, inset, SCREEN_W - inset * 2, SCREEN_H - inset * 2};
            SDL_RenderDrawRect(ren, &border);
        }
    }
    if (snowOn) {
        SDL_SetRenderDrawColor(ren, 240, 248, 255, 205);
        for (int i = 0; i < 56; ++i) {
            int x = (i * 173 + (int)(overlayNow / 31) * (1 + i % 3)) % SCREEN_W;
            int y = ((i * 97 + (int)(overlayNow / (9 + i % 5))) % (SCREEN_H + 20)) - 10;
            SDL_Rect flake{x, y, 1 + i % 3, 1 + i % 3};
            SDL_RenderFillRect(ren, &flake);
        }
    }
    if (sparksOn) {
        for (int i = 0; i < 30; ++i) {
            int life = (int)((overlayNow / 12 + i * 31) % 120);
            if (life > 50) continue;
            int x = (i * 211 + (int)(overlayNow / 7)) % SCREEN_W;
            int y = SCREEN_H - (i * 43 + life * 5) % (SCREEN_H + 40);
            SDL_SetRenderDrawColor(ren, 255, 196 + i % 59, 70, (Uint8)(220 - life * 3));
            SDL_RenderDrawLine(ren, x, y, x - 3 - i % 5, y + 8 + i % 7);
        }
    }
    if (flamesOn) {
        for (int x = 0; x < SCREEN_W; x += 12) {
            int wave = (int)((overlayNow / 18 + x * 7) % 34);
            int h = 18 + wave;
            SDL_SetRenderDrawColor(ren, 255, 76 + wave * 3, 0, 70);
            SDL_Rect flame{x, SCREEN_H - h, 10, h};
            SDL_RenderFillRect(ren, &flame);
        }
    }
    if (eyeMaskHeight > 0) {
        SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
        SDL_Rect upper{0, 0, SCREEN_W, eyeMaskHeight};
        SDL_Rect lower{0, SCREEN_H - eyeMaskHeight, SCREEN_W, eyeMaskHeight};
        SDL_RenderFillRect(ren, &upper);
        SDL_RenderFillRect(ren, &lower);
    }
    // persistent black
    if (blackAlpha > 0) {
        SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(ren, 0, 0, 0, blackAlpha);
        SDL_RenderFillRect(ren, nullptr);
    }
    // white flash
    if (flashAlpha > 0) {
        SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(ren, 255, 255, 255, flashAlpha);
        SDL_RenderFillRect(ren, nullptr);
    }
    // Fixed gfStory-en dialog: 42em wide, 2em from bottom, 24px name rail,
    // 5em text viewport. Typewriter timing is handled by the event loop.
    if (sayEv && sayEv != (const json*)1) {
        const auto& ev = *sayEv;
        std::string name = ev.value("name", "");
        const auto& pages = ev["text"];
        int page = std::min(sayPage, (int)pages.size() - 1);
        std::string text = pages[page].get<std::string>();
        drawDialog(name, text, visibleChars, textAnimating);
    }
    if (choiceEv) drawChoices();
    if (!showLog && !showScript) drawToolbar();
    if (showLog) drawHistory();
    if (showScript) drawScript();
    SDL_RenderPresent(ren);
}

void Player::drawChoices() {
    const auto& options = (*choiceEv)["options"];
    if (options.empty()) return;
    // StoryScene.vue: 75% width (max 42em), 0.8em padding, 5px margin,
    // vertically centered inside the screen minus the 10em dialog area.
    const int width = std::min((int)(SCREEN_W * 0.75f), 672);
    const int rowH = 42;
    const int gap = 10;
    const int totalH = (int)options.size() * rowH + ((int)options.size() - 1) * gap;
    const int x = (SCREEN_W - width) / 2;
    const int y0 = (SCREEN_H - 160 - totalH) / 2;
    SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
    for (int i = 0; i < (int)options.size(); ++i) {
        SDL_Rect box{x, y0 + i * (rowH + gap), width, rowH};
        SDL_SetRenderDrawColor(ren, 0, 0, 0, i == choiceIndex ? 170 : 102);
        SDL_RenderFillRect(ren, &box);
        SDL_SetRenderDrawColor(ren, 253, 179, 0, 192);
        SDL_RenderDrawRect(ren, &box);
        SDL_Rect mark{box.x + 5, box.y + 5, 5, 5};
        SDL_RenderFillRect(ren, &mark);
        std::string option = options[i].get<std::string>();
        drawText(displayText(option), markupFont(option), markupColor(option), box.x + 14, box.y + 11);
    }
}

void Player::captureShot() {
    const char* path = SDL_getenv("GFLVN_SHOT");
    if (shotDone || !path) return;
    shotDone = true;
    SDL_Surface* shot = SDL_CreateRGBSurfaceWithFormat(0, SCREEN_W, SCREEN_H, 32,
                                                       SDL_PIXELFORMAT_RGBA32);
    SDL_RenderReadPixels(ren, nullptr, SDL_PIXELFORMAT_RGBA32, shot->pixels, shot->pitch);
    IMG_SavePNG(shot, path);
    SDL_FreeSurface(shot);
}

std::vector<std::string> Player::wrapText(const std::string& text, int maxW, TTF_Font* face) {
    if (!face) face = font;
    std::vector<std::string> lines;
    std::string cur;
    std::istringstream iss(text);
    std::string word;
    while (iss >> word) {
        std::string trial = cur.empty() ? word : cur + " " + word;
        int tw = 0;
        TTF_SizeUTF8(face, trial.c_str(), &tw, nullptr);
        if (tw > maxW && !cur.empty()) { lines.push_back(cur); cur = word; }
        else cur = trial;
    }
    lines.push_back(cur);
    return lines;
}

int Player::utf8Length(const std::string& text) const {
    int n = 0;
    for (unsigned char c : text) if ((c & 0xc0) != 0x80) n++;
    return n;
}

std::string Player::utf8Prefix(const std::string& text, int chars) const {
    if (chars <= 0) return "";
    int n = 0;
    size_t end = 0;
    while (end < text.size()) {
        if (((unsigned char)text[end] & 0xc0) != 0x80 && n++ >= chars) break;
        end++;
    }
    return text.substr(0, end);
}

std::string Player::displayText(const std::string& text) const {
    std::string out;
    bool tag = false;
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '<') { tag = true; continue; }
        if (tag) { if (text[i] == '>') tag = false; continue; }
        if (i + 2 < text.size() && text.compare(i, 3, "//n") == 0) {
            out += ' '; i += 2; continue;
        }
        out += text[i];
    }
    return out;
}

SDL_Color Player::markupColor(const std::string& text) const {
    size_t p = text.find("<color=#");
    if (p == std::string::npos || p + 14 > text.size()) return {255,255,255,255};
    std::string hex = text.substr(p + 8, 6);
    try {
        unsigned v = std::stoul(hex, nullptr, 16);
        return {(Uint8)(v >> 16), (Uint8)(v >> 8), (Uint8)v, 255};
    } catch (...) { return {255,255,255,255}; }
}

TTF_Font* Player::markupFont(const std::string& text) {
    size_t p = text.find("<size=");
    // StoryScene.vue applies Noto Sans SC to the whole dialog, including Latin.
    bool cjk = true;
    if (p == std::string::npos) return cjk ? cjkFont : font;
    size_t end = text.find('>', p);
    if (end == std::string::npos) return font;
    int value = 50;
    try { value = std::stoi(text.substr(p + 6, end - p - 6)); } catch (...) { return font; }
    int px = std::clamp((int)std::round(18.0 * value / 50.0), 9, 36);
    int cacheKey = cjk ? 1000 + px : px;
    auto it = scaledFonts.find(cacheKey);
    if (it != scaledFonts.end()) return it->second;
    TTF_Font* f = TTF_OpenFont((cjk ? cjkFontPath : fontPath).c_str(), px);
    scaledFonts[cacheKey] = f ? f : (cjk ? cjkFont : font);
    return scaledFonts[cacheKey];
}

bool Player::containsCjk(const std::string& text) const {
    for (size_t i = 0; i < text.size();) {
        unsigned char c = text[i];
        unsigned cp = 0; int n = 1;
        if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; n = 2; }
        else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; n = 3; }
        else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; n = 4; }
        else { cp = c; }
        for (int j = 1; j < n && i + j < text.size(); ++j)
            cp = (cp << 6) | ((unsigned char)text[i + j] & 0x3F);
        if ((cp >= 0x3400 && cp <= 0x9FFF) || (cp >= 0xF900 && cp <= 0xFAFF)) return true;
        i += n;
    }
    return false;
}

void Player::drawText(const std::string& text, TTF_Font* face, SDL_Color color, int x, int y) {
    if (text.empty()) return;
    if (containsCjk(text)) {
        if (face == font) face = cjkFont;
        else if (face == nameFont) face = cjkNameFont;
        else if (face == uiFont) face = cjkUiFont;
    }
    SDL_Surface* s = TTF_RenderUTF8_Blended(face, text.c_str(), color);
    if (!s) return;
    SDL_Texture* t = SDL_CreateTextureFromSurface(ren, s);
    SDL_Rect r{ x, y, s->w, s->h };
    SDL_RenderCopy(ren, t, nullptr, &r);
    SDL_DestroyTexture(t);
    SDL_FreeSurface(s);
}

void Player::drawTextPrefix(const std::string& text, int visible, TTF_Font* face,
                            SDL_Color color, int x, int y) {
    if (text.empty()) return;
    if (containsCjk(text)) {
        if (face == font) face = cjkFont;
        else if (face == nameFont) face = cjkNameFont;
        else if (face == uiFont) face = cjkUiFont;
    }
    SDL_Surface* s = TTF_RenderUTF8_Blended(face, text.c_str(), color);
    if (!s) return;
    SDL_Texture* texture = SDL_CreateTextureFromSurface(ren, s);
    const std::string prefix = utf8Prefix(text, visible);
    int revealW = 0;
    TTF_SizeUTF8(face, prefix.c_str(), &revealW, nullptr);
    revealW = std::clamp(revealW, 0, s->w);
    if (revealW > 0) {
        SDL_Rect src{0, 0, revealW, s->h};
        SDL_Rect dst{x, y, revealW, s->h};
        SDL_RenderCopy(ren, texture, &src, &dst);
    }
    SDL_DestroyTexture(texture);
    SDL_FreeSurface(s);
}

void Player::drawDialog(const std::string& name, const std::string& fullText,
                        int visible, bool animating) {
    // CSS source values at the browser default 16px root size.
    constexpr int boxW = 674;        // 42em content + two 1px borders
    constexpr int boxH = 144;        // measured 143.906px at a 960x544 viewport
    constexpr int bottom = 32;       // margin-bottom: 2em
    constexpr int cutY = 18;
    constexpr int cutX = 240;
    const int bx = (SCREEN_W - boxW) / 2;
    const int by = SCREEN_H - bottom - boxH;
    const int contentX = bx + 1;

    SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
    // Clipped, semi-transparent radial-dot panel. The top-right cut follows
    // polygon(... 100% 18px, 258px 18px, 240px 0).
    SDL_SetRenderDrawColor(ren, 0, 0, 0, 170);
    SDL_Rect body{ bx, by + cutY, boxW, boxH - cutY };
    SDL_RenderFillRect(ren, &body);
    for (int y = 0; y < cutY; y++) {
        SDL_Rect row{ bx, by + y, cutX + y, 1 };
        SDL_RenderFillRect(ren, &row);
    }
    // The CSS radial point has zero radius and is sub-pixel at 960x544; on a
    // black scene it rasterizes to black, not to a grid of bright 1px dots.
    SDL_SetRenderDrawColor(ren, 204, 204, 204, 170);
    SDL_RenderDrawLine(ren, bx, by, bx + cutX, by);
    SDL_RenderDrawLine(ren, bx + cutX, by, bx + cutX + cutY, by + cutY);
    SDL_RenderDrawLine(ren, bx + cutX + cutY, by + cutY, bx + boxW - 1, by + cutY);
    SDL_RenderDrawLine(ren, bx, by, bx, by + boxH - 1);
    SDL_RenderDrawLine(ren, bx, by + boxH - 1, bx + boxW - 1, by + boxH - 1);
    SDL_RenderDrawLine(ren, bx + boxW - 1, by + cutY, bx + boxW - 1, by + boxH - 1);

    // narrator-box + narrator-corner from StoryScene.vue.
    // Do not scale rasterized glyphs. Scaling the whole word to a rounded
    // integer width mangled narrow stems (notably n/o at Vita resolution).
    drawText(name, cjkNameFont, SDL_Color{255, 255, 255, 255}, contentX + 20, by + 3);
    const int railX = bx + 238;
    // Exact intersection of narrator-corner's diagonal polygon with the
    // dialog's own top-right clip. Before y=18 only its two-pixel grey edge is
    // visible; below the cut the orange rail opens to the right.
    for (int y = 0; y < 24; y++) {
        int left = railX + y;
        int right = y < cutY ? bx + cutX + y : bx + boxW - 1;
        if (right < left) continue;
        SDL_SetRenderDrawColor(ren, y < 18 ? 204 : 253, y < 18 ? 204 : 179,
                               y < 18 ? 204 : 0, y < 18 ? 170 : 192);
        SDL_RenderDrawLine(ren, left, by + y, right, by + y);
    }

    const std::string displayed = displayText(fullText);
    TTF_Font* bodyFont = markupFont(fullText);
    // Wrap the final text, then reveal a clipped full-line texture. Previous
    // pixels never move and words do not jump when a later character wraps.
    auto lines = wrapText(displayed, 630, bodyFont);
    int lineH = TTF_FontLineSkip(bodyFont);
    int ty = by + 37;
    int consumed = 0;
    for (const auto& line : lines) {
        if (ty + lineH > by + 122) break; // measured 88px text viewport
        const int lineChars = utf8Length(line);
        const int lineVisible = std::clamp(visible - consumed, 0, lineChars);
        if (lineVisible >= lineChars)
            drawText(line, bodyFont, markupColor(fullText), contentX + 21, ty);
        else if (lineVisible > 0)
            drawTextPrefix(line, lineVisible, bodyFont, markupColor(fullText), contentX + 21, ty);
        consumed += lineChars + 1; // wrapped whitespace between lines
        ty += lineH;
    }

    // The original SVGs in StoryScene.vue, at their exact CSS/natural sizes.
    Tex* logo = getTex("ui_gf_system");
    Tex* loaded = getTex("ui_loaded_circle");
    SDL_Rect logoRect{bx + boxW - 7 - 85, by + boxH - 15, 85, 15};
    if (!animating) {
        SDL_Rect loadedRect{logoRect.x + 85 - 6 - 12, logoRect.y - 12, 12, 12};
        if (loaded->tex) SDL_RenderCopy(ren, loaded->tex, nullptr, &loadedRect);
    }
    if (logo->tex) SDL_RenderCopy(ren, logo->tex, nullptr, &logoRect);
}

// gfStory-en top-left button strip: Menu / Script / Log / Auto
void Player::drawToolbar() {
    static const SDL_Rect btns[4] = {
        { 19, 8, 45, 45 }, { 80, 8, 45, 45 }, { 141, 8, 45, 45 }, { 202, 8, 45, 45 }
    };
    static const char* labels[4] = { "Menu", "Script", "Log", "Auto" };
    SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
    for (int i = 0; i < 4; i++) {
        const SDL_Rect& b = btns[i];
        bool active = (i == 1 && showScript) || (i == 2 && showLog) || (i == 3 && autoMode);
        SDL_SetRenderDrawColor(ren, 0, 0, 0, 0); // transparent CSS background
        SDL_RenderFillRect(ren, &b);
        SDL_SetRenderDrawColor(ren, active ? 255 : 255, active ? 165 : 255, active ? 0 : 255, 136);
        SDL_RenderDrawRect(ren, &b);
        SDL_SetRenderDrawColor(ren, 235, 235, 235, 230);
        float cx = b.x + 15.0f, cy = b.y + 15.0f;
        if (i == 0) {                      // hamburger
            for (int k = -1; k <= 1; k++) {
                SDL_Rect l{ (int)cx - 11, (int)cy + k * 6 - 1, 22, 2 };
                SDL_RenderFillRect(ren, &l);
            }
        } else if (i == 1) {               // script page
            SDL_Rect pg{ (int)cx - 9, (int)cy - 11, 18, 23 };
            SDL_RenderDrawRect(ren, &pg);
            for (int k = 0; k < 3; k++) {
                SDL_Rect l{ (int)cx - 6, (int)cy - 7 + k * 6, 12, 2 };
                SDL_RenderFillRect(ren, &l);
            }
        } else if (i == 2) {               // clock
            for (int a = 0; a < 360; a += 20)
                SDL_RenderDrawPoint(ren, (int)(cx + 10 * cos(a * M_PI / 180)), (int)(cy + 10 * sin(a * M_PI / 180)));
            SDL_RenderDrawLine(ren, (int)cx, (int)cy, (int)cx, (int)cy - 6);
            SDL_RenderDrawLine(ren, (int)cx, (int)cy, (int)cx + 5, (int)cy + 2);
        } else {                           // play triangle
            for (int dy = -7; dy <= 7; dy++) {
                int w = 7 - abs(dy);
                SDL_RenderDrawLine(ren, (int)cx - 4, (int)cy + dy, (int)cx - 4 + w, (int)cy + dy);
            }
        }
        SDL_Surface* ts = TTF_RenderUTF8_Blended(uiFont, labels[i], active ? SDL_Color{255,165,0,255} : SDL_Color{255,255,255,255});
        SDL_Texture* tt = SDL_CreateTextureFromSurface(ren, ts);
        SDL_Rect r{ b.x + b.w - ts->w - 3, b.y + b.h - ts->h, ts->w, ts->h };
        SDL_RenderCopy(ren, tt, nullptr, &r);
        SDL_DestroyTexture(tt); SDL_FreeSurface(ts);
    }
    if (autoMode) {
        drawText("X" + std::to_string(autoSpeed), uiFont, SDL_Color{255,255,255,180}, 263, 1);
        SDL_SetRenderDrawColor(ren, 255, 255, 255, 190);
        SDL_RenderDrawLine(ren, 263, 30, 353, 30);
        int knobX = 263 + (autoSpeed - 1) * 10;
        SDL_Rect knob{ knobX, 21, 12, 18 };
        SDL_RenderFillRect(ren, &knob);
    }
}

void Player::drawHistory() {
    // gfStory-en keeps the scene visible and expands only the dialog text area.
    const int boxW = 674, boxH = 470, bx = (SCREEN_W - boxW) / 2, by = 42;
    SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(ren, 0, 0, 0, 210);
    SDL_Rect body{ bx, by + 18, boxW, boxH - 18 };
    SDL_RenderFillRect(ren, &body);
    for (int y = 0; y < 18; y++) {
        SDL_Rect row{ bx, by + y, 240 + y, 1 };
        SDL_RenderFillRect(ren, &row);
    }
    SDL_SetRenderDrawColor(ren, 204, 204, 204, 170);
    SDL_RenderDrawLine(ren, bx, by, bx + 240, by);
    SDL_RenderDrawLine(ren, bx + 240, by, bx + 258, by + 18);
    SDL_RenderDrawLine(ren, bx + 258, by + 18, bx + boxW - 1, by + 18);
    SDL_RenderDrawLine(ren, bx, by, bx, by + boxH - 1);
    SDL_RenderDrawLine(ren, bx, by + boxH - 1, bx + boxW - 1, by + boxH - 1);
    SDL_RenderDrawLine(ren, bx + boxW - 1, by + 18, bx + boxW - 1, by + boxH - 1);

    int rowH = TTF_FontLineSkip(font) + 4, y = by + boxH - 26;
    const int textW = 630;
    int newest = std::max(-1, (int)history.size() - 1 - historyScroll);
    for (int i = newest; i >= 0 && y > by + 32; i--) {
        auto& [name, text] = history[i];
        const int nameW = 120;
        auto lines = wrapText(text, textW - nameW);
        for (int li = (int)lines.size() - 1; li >= 0 && y > by + 32; li--) {
            drawText(lines[li], font, SDL_Color{255,255,255,255}, bx + 22 + nameW, y - rowH);
            if (li == 0 && !name.empty())
                drawText(name, font, SDL_Color{255,255,255,255}, bx + 22, y - rowH);
            y -= rowH;
        }
        y -= 5;
    }
}

void Player::drawScript() {
    // gfStory-en's Script button opens the complete plain-text script in a
    // modal. Keep it separate from both the story menu and the played-lines
    // history, and make it scrollable with the d-pad.
    const int boxW = 840, boxH = 490, bx = (SCREEN_W - boxW) / 2, by = 27;
    SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(ren, 12, 12, 15, 242);
    SDL_Rect body{bx, by, boxW, boxH};
    SDL_RenderFillRect(ren, &body);
    SDL_SetRenderDrawColor(ren, 204, 204, 204, 180);
    SDL_RenderDrawRect(ren, &body);
    drawText("Script", cjkNameFont, SDL_Color{255,255,255,255}, bx + 20, by + 13);
    drawText("Up/Down: scroll   Circle: close", uiFont,
             SDL_Color{160,160,168,255}, bx + boxW - 230, by + 18);

    std::vector<std::pair<std::string, bool>> lines;
    for (const auto& ev : scene.value("events", json::array())) {
        if (ev.value("t", "") != "say") continue;
        std::string name = ev.value("name", "");
        if (!name.empty()) lines.push_back({name, true});
        for (const auto& page : ev.value("text", json::array())) {
            for (const auto& wrapped : wrapText(displayText(page.get<std::string>()), boxW - 42, cjkFont))
                lines.push_back({wrapped, false});
        }
        lines.push_back({"", false});
    }
    const int lineH = TTF_FontLineSkip(cjkFont) + 2;
    const int rows = std::max(1, (boxH - 58) / lineH);
    const int maxScroll = std::max(0, (int)lines.size() - rows);
    scriptScroll = std::clamp(scriptScroll, 0, maxScroll);
    int y = by + 47;
    for (int i = scriptScroll; i < (int)lines.size() && i < scriptScroll + rows; ++i) {
        SDL_Color color = lines[i].second ? SDL_Color{253,179,0,255} : SDL_Color{245,245,248,255};
        drawText(lines[i].first, lines[i].second ? cjkNameFont : cjkFont, color, bx + 20, y);
        y += lineH;
    }
    if (maxScroll > 0) {
        const int trackH = boxH - 64;
        SDL_SetRenderDrawColor(ren, 255,255,255,45);
        SDL_Rect track{bx + boxW - 8, by + 48, 3, trackH};
        SDL_RenderFillRect(ren, &track);
        const int thumbH = std::max(18, trackH * rows / (int)lines.size());
        const int thumbY = track.y + (trackH - thumbH) * scriptScroll / maxScroll;
        SDL_SetRenderDrawColor(ren, 99,226,183,210);
        SDL_Rect thumb{track.x, thumbY, 3, thumbH};
        SDL_RenderFillRect(ren, &thumb);
    }
}

int Player::toolbarHit(int mx, int my) {
    for (int i = 0; i < 4; i++) {
        SDL_Rect b{ 19 + i * 61, 8, 45, 45 };
        if (mx >= b.x && mx < b.x + b.w && my >= b.y && my < b.y + b.h) return i;
    }
    return -1;
}

void Player::loadProgress() {
    if (progressPath.empty()) return;
    std::ifstream file(progressPath);
    if (!file) return;
    try {
        json data; file >> data;
        for (const auto& id : data.value("started", json::array())) startedScenes.insert(id.get<std::string>());
        for (const auto& id : data.value("read", json::array())) readScenes.insert(id.get<std::string>());
    } catch (const std::exception& e) {
        std::cerr << "WARN invalid progress file: " << e.what() << "\n";
    }
}

void Player::saveProgress() {
    if (autoAdvance || progressPath.empty()) return;
    json data = {{"version", 1}, {"started", startedScenes}, {"read", readScenes}};
    std::ofstream file(progressPath, std::ios::trunc);
    if (file) file << data.dump(1) << "\n";
}

int Player::readStatus(const std::vector<std::string>& scenes) const {
    if (scenes.empty()) return 0;
    const bool allRead = std::all_of(scenes.begin(), scenes.end(), [&](const std::string& id) {
        return readScenes.count(id) != 0;
    });
    if (allRead) return 2;
    const bool anyStarted = std::any_of(scenes.begin(), scenes.end(), [&](const std::string& id) {
        return startedScenes.count(id) != 0 || readScenes.count(id) != 0;
    });
    return anyStarted ? 1 : 0;
}

void Player::toggleRead(const std::vector<std::string>& scenes) {
    if (scenes.empty()) return;
    if (readStatus(scenes) == 2) {
        for (const auto& id : scenes) { readScenes.erase(id); startedScenes.erase(id); }
    } else {
        for (const auto& id : scenes) { readScenes.insert(id); startedScenes.insert(id); }
    }
    saveProgress();
}

void Player::setup() {
#ifdef GFLVN_VITA
    // The rear panel has no function in this VN. Disable it at the SDL Vita
    // backend before touch initialization, and prevent one front tap from
    // arriving twice as both a finger event and a synthesized mouse click.
    SDL_setenv("VITA_DISABLE_TOUCH_BACK", "1", 1);
    SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");
    SDL_SetHint(SDL_HINT_ENABLE_SCREEN_KEYBOARD, "1");

    // SDL's Vita IME backend expects the application to initialize these
    // services. Without them SDL_StartTextInput silently fails and leaves a
    // focused search field with no system keyboard on screen.
    SceAppUtilInitParam appUtilInit{};
    SceAppUtilBootParam appUtilBoot{};
    const int appUtilResult = sceAppUtilInit(&appUtilInit, &appUtilBoot);
    if (appUtilResult < 0)
        std::cerr << "WARN: sceAppUtilInit failed: " << appUtilResult << "\n";
    SceCommonDialogConfigParam dialogConfig;
    sceCommonDialogConfigParamInit(&dialogConfig);
    const int dialogResult = sceCommonDialogSetConfigParam(&dialogConfig);
    if (dialogResult < 0)
        std::cerr << "WARN: sceCommonDialogSetConfigParam failed: " << dialogResult << "\n";
#endif
    CHECK(SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) == 0, "SDL_Init");
    // Character atlases are normally 1024/2048 px square and are reduced to
    // Vita's 544 px height. SDL's nearest-neighbour default made their edges
    // visibly blocky despite the high-resolution source artwork.
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1");
    CHECK((win = SDL_CreateWindow("Girl's Frontline", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                  SCREEN_W, SCREEN_H, 0)) != nullptr, "window");
    CHECK((ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC)) != nullptr ||
          (ren = SDL_CreateRenderer(win, -1, 0)) != nullptr, "renderer");
    // 4096 samples (~93 ms at 44.1 kHz) absorbs short Vita PNG decode spikes
    // without making this non-interactive VN feel laggy.
    audioReady = Mix_OpenAudio(44100, MIX_DEFAULT_FORMAT, 2, 4096) == 0;
    if (!audioReady) std::cerr << "WARN: audio disabled: " << Mix_GetError() << "\n";
    CHECK(TTF_Init() == 0, "ttf");
    font = TTF_OpenFont(fontPath.c_str(), 18);      // CSS 1.1em at 16px
    nameFont = TTF_OpenFont(fontPath.c_str(), 19); // narrator span 1.2em
    uiFont = TTF_OpenFont(fontPath.c_str(), 12);
    cjkFont = TTF_OpenFont(cjkFontPath.c_str(), 18);
    cjkNameFont = TTF_OpenFont(cjkFontPath.c_str(), 19);
    cjkUiFont = TTF_OpenFont(cjkFontPath.c_str(), 12);
    CHECK(font && nameFont && uiFont && cjkFont && cjkNameFont && cjkUiFont, "font load");
#ifdef GFLVN_VITA
    SDL_GameControllerOpen(0);   // vita buttons arrive as controller events
#endif
    // app0: file access is reliable after SDL and the Vita modules are fully
    // initialized. The bootstrap manifest/fonts stay loose; story and media
    // are loaded through the seekable pack from here onwards.
    loadPackIndex();
    autoAdvance = SDL_getenv("GFLVN_AUTO") != nullptr;
    if (!autoAdvance) {
#ifdef GFLVN_VITA
        // SDL_GetPrefPath is not implemented consistently by Vita SDL builds
        // (Vita3K can return no path at all).  Use the conventional per-title
        // ux0 data directory so read/started state survives app restarts on
        // both real hardware and the emulator.
        const std::string saveDir = "ux0:data/GFLVN0001";
        sceIoMkdir(saveDir.c_str(), 0777); // EEXIST is harmless
        progressPath = saveDir + "/progress.json";
        loadProgress();
#else
        char* pref = SDL_GetPrefPath("GFLVN", "GirlsFrontline");
        if (pref) {
            progressPath = std::string(pref) + "progress.json";
            SDL_free(pref);
            loadProgress();
        }
#endif
    }
}

int Player::run() {

    const auto& events = scene["events"];
    size_t pc = 0;
    int sayPage = 0;
    const json* sayEv = nullptr;
    bool running = true;
    int rc = 0;
    int activeBranch = 0;

    while (running && pc < events.size()) {
        const auto& ev = events[pc];
        std::string t = ev.value("t", "");
        if (ev.contains("gate") && ev["gate"].get<int>() != activeBranch) {
            pc++;
            continue;
        }
        if (t == "start") { pc++; continue; }
        if (t == "end") break;

        if (t == "bg") {
            int delayMs = ev.value("delay_ms", 0);
            if (delayMs > 0) {
                drawAll(sayPage, sayEv);
                SDL_Delay((Uint32)delayMs);
            }
            bgId = ev["id"];
            if (ev.value("transition", "") == "slow_fade") {
                for (int a = 255; a >= 0; a -= 4) {
                    blackAlpha = (Uint8)a;
                    drawAll(sayPage, nullptr);
                    SDL_Delay(16);
                }
                blackAlpha = 0;
            } else if (blackAlpha > 0 && !ev.contains("transition")) {
                for (int a = blackAlpha; a >= 0; a -= 8) {
                    blackAlpha = (Uint8)a;
                    drawAll(sayPage, sayEv);
                    SDL_Delay(16); // CSS background opacity transition: 0.5s
                }
                blackAlpha = 0;
            }
        } else if (t == "music") {
            playMusic(ev["id"]);
        } else if (t == "sfx") {
            if (ev["id"] == "se_Stop_AVG_loop" || ev["id"] == "se_stop_AVG_applause_indoor") {
                if (audioReady) Mix_HaltChannel(-1);
                pc++;
                continue;
            }
            playSfx(ev["id"]);
        } else if (t == "show" || t == "hide") {
            // These are useful IR diffs, but visible state is committed only by
            // the absolute `stage` snapshot on the following say/choice event.
            // Rendering each diff exposed a one-frame, centrally re-laid-out
            // cast when the player advanced through text quickly.
        } else if (t == "night") {
            nightOn = ev.value("on", false);
        } else if (t == "effect") {
            std::string k = ev.value("kind", "");
            if (k == "black_on") blackAlpha = 255;
            else if (k == "fade_from_black") {
                for (int a = 255; a >= 0; a -= 4) { blackAlpha = a; drawAll(sayPage, sayEv); SDL_Delay(16); }
                blackAlpha = 0;
            } else if (k == "shake") {
                shakeUntil = SDL_GetTicks() + 600;
                while (SDL_GetTicks() < shakeUntil) { drawAll(sayPage, sayEv); SDL_Delay(16); }
                shakeUntil = 0;
            } else if (k == "flash" || k == "flash_white" || k == "blink") {
                for (int a = 220; a >= 0; a -= 20) { flashAlpha = (Uint8)a; drawAll(sayPage, sayEv); SDL_Delay(16); }
                flashAlpha = 0;
            } else if (k == "eyes_open") {
                // The preceding bg event has already selected the image, but
                // transition=eyes_open deliberately left the persistent black
                // mask in place. Reveal the image through the eyelid aperture
                // instead of fading the full background before this effect.
                blackAlpha = 0;
                for (int h = SCREEN_H / 2; h >= 0; h -= 10) {
                    eyeMaskHeight = h;
                    drawAll(sayPage, nullptr);
                    SDL_Delay(16);
                }
                eyeMaskHeight = 0;
            } else if (k == "memory_mask_on") {
                memoryMaskOn = true;
            } else if (k == "masks_off") {
                memoryMaskOn = false;
            } else if (k == "snow_on") {
                snowOn = true;
            } else if (k == "sparks_on") {
                sparksOn = true;
            } else if (k == "sparks_off") {
                sparksOn = false;
            } else if (k == "flames_on") {
                flamesOn = true;
            } else if (k == "particles_off") {
                snowOn = sparksOn = flamesOn = false;
            }
        } else if (t == "say") {
            if (ev.contains("stage")) syncStage(ev["stage"]);
            sayEv = &ev; sayPage = 0; visibleChars = 0; textAnimating = true;
        } else if (t == "choice") {
            if (ev.contains("stage")) syncStage(ev["stage"]);
            choiceEv = &ev;
            choiceIndex = 0;
        }

        // gfStory-en consumes background/audio/class updates until it reaches
        // a text or selection boundary. Do the same: never present transient
        // show/hide states between two user-visible lines.
        if (t == "say" || t == "choice") drawAll(sayPage, sayEv);

        if (t == "say" && SDL_getenv("GFLVN_SHOT_CALL_AT") &&
            std::any_of(stage.begin(), stage.end(), [](const Sprite& s){ return s.remote && !s.leaving; })) {
            SDL_Delay((Uint32)std::max(0, std::atoi(SDL_getenv("GFLVN_SHOT_CALL_AT"))));
            drawAll(sayPage, sayEv);
            captureShot();
        }

        if (sayEv && !SDL_getenv("GFLVN_SHOT_REMOTE") && !SDL_getenv("GFLVN_SHOT_CALL_AT") &&
            !SDL_getenv("GFLVN_SHOT_AT_CHARS")) captureShot();

        if (t == "choice") {
            bool chosen = false;
            if (autoAdvance) {
                activeBranch = 1;
                chosen = true;
            }
            while (!chosen && running) {
                SDL_Event e;
                while (SDL_PollEvent(&e)) {
                    if (e.type == SDL_QUIT) { running = false; rc = -1; break; }
                    if (e.type == SDL_KEYDOWN) {
                        if (e.key.keysym.sym == SDLK_ESCAPE) { running = false; rc = -2; break; }
                        if (e.key.keysym.sym == SDLK_UP) choiceIndex--;
                        else if (e.key.keysym.sym == SDLK_DOWN) choiceIndex++;
                        else if (e.key.keysym.sym >= SDLK_1 && e.key.keysym.sym <= SDLK_9)
                            choiceIndex = e.key.keysym.sym - SDLK_1;
                        else if (e.key.keysym.sym == SDLK_RETURN || e.key.keysym.sym == SDLK_SPACE ||
                                 e.key.keysym.sym == SDLK_x) chosen = true;
                    }
#ifdef GFLVN_VITA
                    else if (e.type == SDL_CONTROLLERBUTTONDOWN) {
                        if (e.cbutton.button == SDL_CONTROLLER_BUTTON_DPAD_UP) choiceIndex--;
                        else if (e.cbutton.button == SDL_CONTROLLER_BUTTON_DPAD_DOWN) choiceIndex++;
                        else if (e.cbutton.button == SDL_CONTROLLER_BUTTON_A) chosen = true;
                        else if (e.cbutton.button == SDL_CONTROLLER_BUTTON_START) { running = false; rc = -2; }
                    }
#endif
                    else if (e.type == SDL_MOUSEBUTTONDOWN || isFrontFingerDown(e)) {
                        int mx = e.type == SDL_MOUSEBUTTONDOWN ? e.button.x : (int)(e.tfinger.x * SCREEN_W);
                        int my = e.type == SDL_MOUSEBUTTONDOWN ? e.button.y : (int)(e.tfinger.y * SCREEN_H);
                        int count = (int)(*choiceEv)["options"].size();
                        int totalH = count * 42 + (count - 1) * 10;
                        int y0 = (SCREEN_H - 160 - totalH) / 2;
                        int row = (my - y0) / 52;
                        if (mx >= (SCREEN_W - std::min((int)(SCREEN_W * .75f), 672)) / 2 &&
                            mx <= (SCREEN_W + std::min((int)(SCREEN_W * .75f), 672)) / 2 &&
                            row >= 0 && row < count && my - y0 - row * 52 < 42) {
                            choiceIndex = row;
                            chosen = true;
                        }
                    }
                    int count = (int)(*choiceEv)["options"].size();
                    choiceIndex = std::clamp(choiceIndex, 0, std::max(0, count - 1));
                    drawAll(sayPage, sayEv);
                }
                SDL_Delay(50);
            }
            if (chosen) activeBranch = choiceIndex + 1;
            choiceEv = nullptr;
        } else if (t == "say") {
            // wait for advance through all pages of this line
            bool lineDone = false;
            autoPageAt = SDL_GetTicks();
            Uint32 charStartedAt = autoPageAt;
            Uint32 lastFrameAt = 0;
            while (!lineDone && running) {
                SDL_Event e;
                while (SDL_PollEvent(&e)) {
                    bool adv = false;
                    if (e.type == SDL_QUIT) { running = false; rc = -1; }
                    else if (e.type == SDL_KEYDOWN) {
                        if (showLog || showScript) {
                            if (e.key.keysym.sym == SDLK_UP) {
                                if (showLog) historyScroll = std::min(std::max(0, (int)history.size() - 1), historyScroll + 1);
                                else scriptScroll = std::max(0, scriptScroll - 1);
                            } else if (e.key.keysym.sym == SDLK_DOWN) {
                                if (showLog) historyScroll = std::max(0, historyScroll - 1);
                                else scriptScroll++;
                            } else if (e.key.keysym.sym == SDLK_ESCAPE || e.key.keysym.sym == SDLK_RETURN ||
                                       e.key.keysym.sym == SDLK_SPACE || e.key.keysym.sym == SDLK_x) {
                                showLog = showScript = false;
                            }
                            drawAll(sayPage, sayEv);
                            continue;
                        }
                        if (e.key.keysym.sym == SDLK_ESCAPE) { running = false; rc = -2; }
                        else if (e.key.keysym.sym == SDLK_SPACE || e.key.keysym.sym == SDLK_RETURN ||
                                 e.key.keysym.sym == SDLK_x) adv = true;
                        else if (e.key.keysym.sym == SDLK_a) { autoMode = !autoMode; autoPageAt = SDL_GetTicks(); drawAll(sayPage, sayEv); }
                    }
#ifdef GFLVN_VITA
                    else if (e.type == SDL_CONTROLLERBUTTONDOWN) {
                        if (showLog || showScript) {
                            if (e.cbutton.button == SDL_CONTROLLER_BUTTON_DPAD_UP) {
                                if (showLog) historyScroll = std::min(std::max(0, (int)history.size() - 1), historyScroll + 1);
                                else scriptScroll = std::max(0, scriptScroll - 1);
                            } else if (e.cbutton.button == SDL_CONTROLLER_BUTTON_DPAD_DOWN) {
                                if (showLog) historyScroll = std::max(0, historyScroll - 1);
                                else scriptScroll++;
                            } else if (e.cbutton.button == SDL_CONTROLLER_BUTTON_B ||
                                       e.cbutton.button == SDL_CONTROLLER_BUTTON_A ||
                                       e.cbutton.button == SDL_CONTROLLER_BUTTON_BACK) {
                                showLog = showScript = false;
                            }
                            drawAll(sayPage, sayEv);
                            continue;
                        }
                        if (e.cbutton.button == SDL_CONTROLLER_BUTTON_START) { running = false; rc = -2; }
                        else if (e.cbutton.button == SDL_CONTROLLER_BUTTON_BACK) {
                            showLog = true; showScript = false; historyScroll = 0; drawAll(sayPage, sayEv);
                        }
                        else if (e.cbutton.button == SDL_CONTROLLER_BUTTON_Y) { autoMode = !autoMode; autoPageAt = SDL_GetTicks(); drawAll(sayPage, sayEv); }
                        else if (e.cbutton.button == SDL_CONTROLLER_BUTTON_A) adv = true;
                    }
#endif
                    else if (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_LEFT) {
                        if (showLog || showScript) {
                            showLog = showScript = false; drawAll(sayPage, sayEv); continue;
                        }
                        int hit = toolbarHit(e.button.x, e.button.y);
                        if (hit == 0) { running = false; rc = -2; }
                        else if (hit == 1) { showScript = true; showLog = false; scriptScroll = 0; drawAll(sayPage, sayEv); }
                        else if (hit == 2) { showLog = true; showScript = false; historyScroll = 0; drawAll(sayPage, sayEv); }
                        else if (hit == 3) { autoMode = !autoMode; autoPageAt = SDL_GetTicks(); drawAll(sayPage, sayEv); }
                        else if (autoMode && e.button.x >= 263 && e.button.x <= 365 && e.button.y >= 8 && e.button.y <= 53) {
                            autoSpeed = std::clamp(1 + (e.button.x - 263) / 10, 1, 10);
                            drawAll(sayPage, sayEv);
                        }
                        else adv = true;
                    }
                    else if (isFrontFingerDown(e)) {
                        if (showLog || showScript) {
                            showLog = showScript = false; drawAll(sayPage, sayEv); continue;
                        }
                        int fx = (int)(e.tfinger.x * SCREEN_W), fy = (int)(e.tfinger.y * SCREEN_H);
                        int hit = toolbarHit(fx, fy);
                        if (hit == 0) { running = false; rc = -2; }
                        else if (hit == 1) { showScript = true; showLog = false; scriptScroll = 0; drawAll(sayPage, sayEv); }
                        else if (hit == 2) { showLog = true; showScript = false; historyScroll = 0; drawAll(sayPage, sayEv); }
                        else if (hit == 3) { autoMode = !autoMode; autoPageAt = SDL_GetTicks(); drawAll(sayPage, sayEv); }
                        else adv = true;
                    }
                    if (adv) {
                        if (textAnimating) {
                            const std::string pageText = displayText((*sayEv)["text"][sayPage].get<std::string>());
                            visibleChars = utf8Length(pageText);
                            textAnimating = false;
                            autoPageAt = SDL_GetTicks();
                        }
                        else {
                            sayPage++;
                            if (sayPage >= (int)(*sayEv)["text"].size()) lineDone = true;
                            else {
                                visibleChars = 0;
                                textAnimating = true;
                                charStartedAt = SDL_GetTicks();
                            }
                        }
                        if (!lineDone) drawAll(sayPage, sayEv);
                    }
                }
                Uint32 now = SDL_GetTicks();
                if (running && !lineDone && !showLog && !showScript && textAnimating) {
                    const std::string pageText = displayText((*sayEv)["text"][sayPage].get<std::string>());
                    int count = utf8Length(pageText);
                    const bool testTypewriter = SDL_getenv("GFLVN_TEST_TYPEWRITER") != nullptr;
                    int nextVisible = autoAdvance && !testTypewriter
                        ? count : std::min(count, (int)((now - charStartedAt) / 42) + 1);
                    if (nextVisible != visibleChars || now - lastFrameAt >= 33) {
                        visibleChars = nextVisible;
                        if (visibleChars >= count) {
                            textAnimating = false;
                            autoPageAt = now; // scheduleAuto starts when AnimatedText finishes
                        }
                        drawAll(sayPage, sayEv);
                        if (const char* shotAt = SDL_getenv("GFLVN_SHOT_AT_CHARS")) {
                            const char* shotName = SDL_getenv("GFLVN_SHOT_NAME");
                            const bool nameMatches = !shotName || (*sayEv).value("name", "") == shotName;
                            if (nameMatches && visibleChars == std::max(0, std::atoi(shotAt))) captureShot();
                        }
                        if (!textAnimating && SDL_getenv("GFLVN_SHOT_REMOTE") &&
                            std::any_of(stage.begin(), stage.end(), [](const Sprite& s){ return s.remote; })) {
                            // Headless auto tests otherwise capture the first frame of
                            // the 200 ms sprite entrance (correctly still transparent).
                            SDL_Delay(400);
                            drawAll(sayPage, sayEv);
                            captureShot();
                        }
                        lastFrameAt = now;
                    }
                }
                if (running && !lineDone && !showLog && !showScript && !textAnimating &&
                    (callSessionActive || snowOn || sparksOn || flamesOn) &&
                    now - lastFrameAt >= 33) {
                    drawAll(sayPage, sayEv);
                    lastFrameAt = now;
                }
                if (running && !lineDone && !showLog && !showScript && !textAnimating && (autoAdvance || autoMode)) {
                    const std::string pageText = displayText((*sayEv)["text"][sayPage].get<std::string>());
                    Uint32 delay = autoAdvance ? 0 : (Uint32)((utf8Length(pageText) / 20.0) * (5000.0 / autoSpeed));
                    if (now - autoPageAt >= delay) {
                        sayPage++;
                        if (sayPage >= (int)(*sayEv)["text"].size()) lineDone = true;
                        else {
                            visibleChars = 0;
                            textAnimating = true;
                            charStartedAt = now;
                        }
                        if (!lineDone) drawAll(sayPage, sayEv);
                    }
                }
                const bool moving = textAnimating || callSessionActive || snowOn || sparksOn || flamesOn;
                SDL_Delay(moving ? 16 : 50);
            }
            // record full line in backlog
            std::string joined;
            for (auto& pg : (*sayEv)["text"]) joined += displayText(pg.get<std::string>()) + " ";
            history.emplace_back((*sayEv).value("name", ""), joined);
            if (history.size() > 200) history.erase(history.begin());
            sayEv = nullptr;
        }
        pc++;
    }
    if (callSessionActive) {
        callSessionActive = false;
        playSfx("se_AVG_tele_disconnect");
    }
    if (rc == 0 && !autoAdvance && !currentSceneId.empty()) {
        startedScenes.insert(currentSceneId);
        readScenes.insert(currentSceneId);
        saveProgress();
    }
    // This runtime returns to story selection after a scene or Menu press.
    // Unlike gfStory's overlaid web drawer, there is no player behind that
    // screen, so carrying its BGM into the menu is both wrong and wasteful.
    if (audioReady) {
        stopMusic();
    }
    std::cout << "scene finished\n";
    return rc;
}

int Player::pickScene(const std::vector<std::string>& names, const std::string& title,
                      const std::vector<std::vector<std::string>>& descendants,
                      const std::vector<std::string>& logos,
                      const std::vector<std::string>& wallpapers,
                      const std::string& fixedLogo, const std::string& fixedWallpaper,
                      bool allowBack) {
    if (autoAdvance && !names.empty()) return 0;  // CI / smoke test
    int sel = 0;
    // gfStory-en uses a left Naive UI drawer: min(80vw, 400px).
    const int rowH = 40, x0 = 0, w = 400, y0 = 108;
    int heldDirection = 0;
    Uint32 nextRepeatAt = 0;
    bool dirty = true;
    bool searchFocused = false;
    std::string query;
    auto closeSearch = [&](bool cancelSystemKeyboard) {
#ifdef GFLVN_VITA
        if (cancelSystemKeyboard && SDL_IsScreenKeyboardShown(win)) {
            sceImeDialogAbort();
            // Let SDL collect and terminate the aborted common dialog before
            // disabling text input; its Vita HideScreenKeyboard cannot abort
            // a dialog which is still in the running state.
            SDL_PumpEvents();
        }
#else
        (void)cancelSystemKeyboard;
#endif
        SDL_StopTextInput();
        searchFocused = false;
        heldDirection = 0;
        dirty = true;
    };
    auto focusSearch = [&]() {
        if (searchFocused) return;
        SDL_StartTextInput();
#ifdef GFLVN_VITA
        searchFocused = SDL_IsScreenKeyboardShown(win);
        if (!searchFocused) {
            std::cerr << "WARN: Vita search keyboard failed to open: " << SDL_GetError() << "\n";
            SDL_StopTextInput();
        }
#else
        searchFocused = true;
#endif
        heldDirection = 0;
        dirty = true;
    };
    auto filteredIndices = [&]() {
        std::vector<int> out;
        std::string needle = query;
        std::transform(needle.begin(), needle.end(), needle.begin(),
                       [](unsigned char c) { return (char)std::tolower(c); });
        for (int i = 0; i < (int)names.size(); ++i) {
            std::string label = names[i];
            std::transform(label.begin(), label.end(), label.begin(),
                           [](unsigned char c) { return (char)std::tolower(c); });
            if (needle.empty() || label.find(needle) != std::string::npos) out.push_back(i);
        }
        return out;
    };
    auto moveSelection = [&](int direction) {
        const int count = (int)filteredIndices().size();
        if (count > 0) {
            sel = (sel + direction + count) % count;
            dirty = true;
        }
    };
    while (true) {
        const std::vector<int> filtered = filteredIndices();
        sel = std::clamp(sel, 0, std::max(0, (int)filtered.size() - 1));
        const int actualSel = filtered.empty() ? -1 : filtered[sel];
        const int visibleRows = std::max(1, (SCREEN_H - y0 - 34) / rowH);
        const int maxFirst = std::max(0, (int)filtered.size() - visibleRows);
        const int first = std::clamp(sel - visibleRows / 2, 0, maxFirst);
        const int last = std::min((int)filtered.size(), first + visibleRows);
        if (dirty) {
        SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
        SDL_RenderClear(ren);
        std::string wallpaper = fixedWallpaper;
        if (actualSel >= 0 && actualSel < (int)wallpapers.size() && !wallpapers[actualSel].empty())
            wallpaper = wallpapers[actualSel];
        if (!wallpaper.empty()) {
            Tex* art = getTex(wallpaper);
            if (art->tex) {
                double scale = std::max((double)SCREEN_W / art->w, (double)SCREEN_H / art->h);
                SDL_Rect dst{(SCREEN_W - (int)(art->w * scale)) / 2,
                             (SCREEN_H - (int)(art->h * scale)) / 2,
                             (int)(art->w * scale), (int)(art->h * scale)};
                SDL_RenderCopy(ren, art->tex, nullptr, &dst);
            }
        }
        SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(ren, 24, 24, 28, wallpaper.empty() ? 255 : 238);
        SDL_Rect drawer{0, 0, w, SCREEN_H};
        SDL_RenderFillRect(ren, &drawer);
        if (!title.empty()) {
            SDL_Surface* ts = TTF_RenderUTF8_Blended(nameFont, title.c_str(), SDL_Color{ 255, 255, 255, 255 });
            SDL_Texture* tt = SDL_CreateTextureFromSurface(ren, ts);
            SDL_Rect r{ 24, 22, ts->w, ts->h };
            SDL_RenderCopy(ren, tt, nullptr, &r);
            SDL_DestroyTexture(tt); SDL_FreeSurface(ts);
        }
        // StoryList's search control and the drawer separator.
        SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(ren, 255, 255, 255, 20);
        SDL_Rect search{24, 60, 352, 34};
        SDL_RenderFillRect(ren, &search);
        SDL_SetRenderDrawColor(ren, searchFocused ? 99 : 255,
                               searchFocused ? 226 : 255,
                               searchFocused ? 183 : 255, searchFocused ? 220 : 48);
        SDL_RenderDrawRect(ren, &search);
        const std::string searchText = query.empty() ? "Search" : query + (searchFocused ? "_" : "");
        drawText(searchText, uiFont, query.empty() ? SDL_Color{145,145,152,255} : SDL_Color{235,235,240,255}, 38, 70);
        SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
        for (int i = first; i < last; i++) {
            const int actual = filtered[i];
            SDL_Rect row{ x0, y0 + (i - first) * rowH, w, rowH };
            bool selected = i == sel && !searchFocused;
            if (selected) {
                SDL_SetRenderDrawColor(ren, 255, 255, 255, 18);
                SDL_RenderFillRect(ren, &row);
                SDL_SetRenderDrawColor(ren, 99, 226, 183, 255);
                SDL_Rect bar{ row.x, row.y, 3, row.h };
                SDL_RenderFillRect(ren, &bar);
            }
            SDL_Color c = selected ? SDL_Color{99,226,183,255} : SDL_Color{220,220,225,255};
            int status = actual < (int)descendants.size() ? readStatus(descendants[actual]) : 0;
            if (actual < (int)descendants.size()) {
                const char* marker = status == 2 ? "[x]" : (status == 1 ? "[~]" : "[ ]");
                drawText(marker, uiFont, status == 2 ? SDL_Color{99,226,183,255} : SDL_Color{145,145,152,255},
                         row.x + 18, row.y + 13);
            }
            SDL_Surface* ts = TTF_RenderUTF8_Blended(uiFont, names[(size_t)actual].c_str(), c);
            SDL_Texture* tt = SDL_CreateTextureFromSurface(ren, ts);
            SDL_Rect r{ row.x + (descendants.empty() ? 24 : 50), row.y + (row.h - ts->h) / 2, ts->w, ts->h };
            SDL_RenderCopy(ren, tt, nullptr, &r);
            SDL_DestroyTexture(tt); SDL_FreeSurface(ts);
        }
        if ((int)filtered.size() > visibleRows) {
            std::string page = std::to_string(sel + 1) + " / " + std::to_string(filtered.size());
            drawText(page, uiFont, SDL_Color{145,145,152,255}, x0 + w - 74, 31);
        }
        if (!descendants.empty())
            drawText("Square: mark read / unread", uiFont, SDL_Color{170,170,178,255}, 24, SCREEN_H - 22);
        std::string logo = fixedLogo;
        if (actualSel >= 0 && actualSel < (int)logos.size() && !logos[actualSel].empty()) logo = logos[actualSel];
        if (!logo.empty()) {
            Tex* art = getTex(logo);
            if (art->tex) {
                double scale = std::min(1.0, std::min(300.0 / art->w, 150.0 / art->h));
                const int logoW = (int)(art->w * scale);
                const int logoH = (int)(art->h * scale);
                // Event posters put faces across the upper half. Keep campaign
                // branding anchored at the bottom-right of the unobscured pane.
                SDL_Rect dst{SCREEN_W - logoW - 24, SCREEN_H - logoH - 20,
                             logoW, logoH};
                SDL_RenderCopy(ren, art->tex, nullptr, &dst);
            }
        }
        SDL_RenderPresent(ren);
        dirty = false;
        if (SDL_getenv("GFLVN_SHOT_MENU")) {
            captureShot();
            return -1;
        }
        }
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) return -1;
            if (e.type == SDL_TEXTINPUT && searchFocused) {
                query += e.text.text;
                sel = 0; dirty = true;
            }
            if (e.type == SDL_KEYDOWN) {
                if (searchFocused && e.key.keysym.sym == SDLK_BACKSPACE && !query.empty()) {
                    size_t start = query.size() - 1;
                    while (start > 0 && ((unsigned char)query[start] & 0xc0) == 0x80) --start;
                    query.erase(start);
                    sel = 0; dirty = true;
                } else if (e.key.keysym.sym == SDLK_UP) {
                    if (sel == 0 && !searchFocused) focusSearch();
                    else if (!searchFocused) { moveSelection(-1); heldDirection = -1; nextRepeatAt = SDL_GetTicks() + 320; }
                } else if (e.key.keysym.sym == SDLK_DOWN) {
                    if (searchFocused) { closeSearch(true); sel = 0; }
                    else { moveSelection(1); heldDirection = 1; nextRepeatAt = SDL_GetTicks() + 320; }
                }
                const std::vector<int> current = filteredIndices();
                int actual = current.empty() ? -1 : current[std::clamp(sel, 0, (int)current.size() - 1)];
                if (e.key.keysym.sym == SDLK_s && !searchFocused && actual >= 0 && actual < (int)descendants.size()) {
                    toggleRead(descendants[actual]); dirty = true;
                }
                if ((e.key.keysym.sym == SDLK_RETURN || e.key.keysym.sym == SDLK_x) && !searchFocused && actual >= 0) return actual;
                if (e.key.keysym.sym == SDLK_ESCAPE) {
                    if (searchFocused) closeSearch(true);
                    else if (allowBack) return -1;
                }
            }
            if (e.type == SDL_KEYUP &&
                ((heldDirection < 0 && e.key.keysym.sym == SDLK_UP) ||
                 (heldDirection > 0 && e.key.keysym.sym == SDLK_DOWN))) heldDirection = 0;
#ifdef GFLVN_VITA
            if (e.type == SDL_CONTROLLERBUTTONDOWN) {
                if (e.cbutton.button == SDL_CONTROLLER_BUTTON_DPAD_UP) {
                    if (sel == 0 && !searchFocused) {
                        focusSearch();
                    } else if (!searchFocused) {
                        moveSelection(-1); heldDirection = -1; nextRepeatAt = SDL_GetTicks() + 320;
                    }
                }
                if (e.cbutton.button == SDL_CONTROLLER_BUTTON_DPAD_DOWN) {
                    if (searchFocused) { closeSearch(true); sel = 0; }
                    else { moveSelection(1); heldDirection = 1; nextRepeatAt = SDL_GetTicks() + 320; }
                }
                const std::vector<int> current = filteredIndices();
                int actual = current.empty() ? -1 : current[std::clamp(sel, 0, (int)current.size() - 1)];
                if (e.cbutton.button == SDL_CONTROLLER_BUTTON_X && !searchFocused && actual >= 0 && actual < (int)descendants.size()) {
                    toggleRead(descendants[actual]); dirty = true;
                }
                if (e.cbutton.button == SDL_CONTROLLER_BUTTON_A && !searchFocused && actual >= 0) return actual;
                if (e.cbutton.button == SDL_CONTROLLER_BUTTON_B) {
                    if (searchFocused) closeSearch(true);
                    else if (allowBack) return -1;
                    else dirty = true; // Circle at the root is intentionally a no-op.
                }
            }
            if (e.type == SDL_CONTROLLERBUTTONUP &&
                ((heldDirection < 0 && e.cbutton.button == SDL_CONTROLLER_BUTTON_DPAD_UP) ||
                 (heldDirection > 0 && e.cbutton.button == SDL_CONTROLLER_BUTTON_DPAD_DOWN)))
                heldDirection = 0;
#endif
            if ((e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_LEFT) ||
                isFrontFingerDown(e)) {
                const int pointerX = e.type == SDL_MOUSEBUTTONDOWN
                    ? e.button.x : (int)(e.tfinger.x * SCREEN_W);
                const int pointerY = e.type == SDL_MOUSEBUTTONDOWN
                    ? e.button.y : (int)(e.tfinger.y * SCREEN_H);
                if (pointerX >= 24 && pointerX < 376 && pointerY >= 60 && pointerY < 94) {
                    focusSearch();
                } else {
                    int idx = first + (pointerY - y0) / rowH;
                    if (idx >= 0 && idx < (int)filtered.size()) return filtered[idx];
                }
            }
        }
#ifdef GFLVN_VITA
        // Accept and Cancel are consumed by the system IME. Once SDL has
        // collected that result, release menu focus even if no key event was
        // delivered to the application.
        if (searchFocused && !SDL_IsScreenKeyboardShown(win)) closeSearch(false);
#endif
        Uint32 now = SDL_GetTicks();
        if (heldDirection != 0 && now >= nextRepeatAt) {
            moveSelection(heldDirection);
            nextRepeatAt = now + 65;
        }
        SDL_Delay(heldDirection ? 10 : 75);
    }
}

// '2first' -> (2, 0)  '2end' -> (2, 2)  '1' -> (1, 1): reading order first < plain < end
static std::pair<int, int> partKey(const std::string& part) {
    int n = 0; size_t i = 0;
    while (i < part.size() && isdigit((unsigned char)part[i])) n = n * 10 + (part[i++] - '0');
    std::string tail = part.substr(i);
    int r = tail.empty() ? 1 : (tail.find("first") != std::string::npos ? 0 : 2);
    return { n, r };
}

// '1-2-2first' -> {chapter "1-2", part "2first"}; 'scene' -> {"0", "scene"}
static std::pair<std::string, std::string> splitSceneId(const std::string& id) {
    size_t p1 = id.find('-');
    if (p1 == std::string::npos) return { "0", id };
    size_t p2 = id.find('-', p1 + 1);
    if (p2 == std::string::npos) return { id, "1" };  // 'X-Y' file: whole thing is the chapter
    return { id.substr(0, p2), id.substr(p2 + 1) };
}

// numeric dash-separated key so "1-10" sorts after "1-9"
static std::vector<int> numPath(const std::string& id) {
    std::vector<int> v;
    int n = 0; bool in = false;
    for (char c : id) {
        if (isdigit((unsigned char)c)) { n = n * 10 + (c - '0'); in = true; }
        else if (in) { v.push_back(n); n = 0; in = false; }
    }
    if (in) v.push_back(n);
    return v;
}

static void appendSceneIds(const json& node, std::vector<std::string>& out) {
    if (node.is_object()) {
        if (node.contains("id") && node["id"].is_string()) out.push_back(node["id"].get<std::string>());
        for (const char* key : {"files", "stories", "chapters"}) {
            if (node.contains(key)) appendSceneIds(node[key], out);
        }
    } else if (node.is_array()) {
        for (const auto& child : node) appendSceneIds(child, out);
    }
}

static std::vector<std::string> sceneIds(const json& node) {
    std::vector<std::string> out;
    appendSceneIds(node, out);
    return out;
}

int main(int argc, char** argv) {
#ifdef GFLVN_VITA
    // Vita launches without argv; data lives in the vpk
    (void)argc;
    (void)argv;
    std::string root = "app0:";
#else
    std::string root = argc > 1 ? argv[1] : "assets";
#endif
    Player p(root);

    // discover scenes: scenes/*.ir.json grouped into chapters, gfStory-en style:
    //   Chapter X-Y -> Part N (first/second). Fallback: single scene.ir.json.
    struct Chap { std::string label; std::vector<std::string> files; std::vector<std::string> labels; };
    std::vector<Chap> chapters;
    std::vector<std::string> availableFiles;
    std::error_code ec;
    for (auto& f : fs::directory_iterator(root + "/scenes", ec)) {
        if (ec || f.path().extension() != ".json") continue;
        std::string fn = f.path().filename().string();
        if (fn.find(".ir.") == std::string::npos) continue;
        availableFiles.push_back(fn);
    }
    if (availableFiles.empty()) availableFiles = p.sceneFileNames();
    for (const auto& fn : availableFiles) {
        std::string id = fn.substr(0, fn.size() - std::string(".ir.json").size()); // e.g. 1-2-2first
        auto [chId, part] = splitSceneId(id);
        auto [pn, pr] = partKey(part);
        std::string plabel = "Part " + std::to_string(pn);
        if (pr == 0) plabel += " (first)";
        if (pr == 2) plabel += " (second)";
        auto it = std::find_if(chapters.begin(), chapters.end(),
                               [&](const Chap& c) { return c.label == chId; });
        if (it == chapters.end()) {
            chapters.push_back({ chId, {}, {} });
            it = chapters.end() - 1;
        }
        it->files.push_back(fn);
        it->labels.push_back(plabel);
    }
    sort(chapters.begin(), chapters.end(), [](const Chap& a, const Chap& b) {
        return numPath(a.label) < numPath(b.label);
    });
    for (auto& c : chapters) {   // order parts within a chapter
        std::vector<size_t> idx(c.files.size());
        for (size_t i = 0; i < idx.size(); i++) idx[i] = i;
        sort(idx.begin(), idx.end(), [&](size_t a, size_t b) {
            return partKey(splitSceneId(c.files[a]).second) < partKey(splitSceneId(c.files[b]).second);
        });
        std::vector<std::string> files, labels;
        for (size_t i : idx) { files.push_back(c.files[i]); labels.push_back(c.labels[i]); }
        c.files = files; c.labels = labels;
    }

    if (chapters.empty()) {
        if (!fs::exists(root + "/scene.ir.json")) { std::cerr << "no scenes found\n"; return 1; }
        chapters.push_back({ "scene", { "scene.ir.json" }, { "scene" } });
    }

    p.setup();
    if (p.isAuto()) {  // smoke test: play every scene once, no menu
        const char* requested = SDL_getenv("GFLVN_SCENE");
        for (const auto& c : chapters)
            for (const auto& fn : c.files) {
                if (requested) {
                    std::string wanted = requested;
                    if (fn != wanted && fn != wanted + ".ir.json") continue;
                }
                p.loadScene(root + "/scenes/" + fn);
                p.run();
            }
        return 0;
    }

    // Use gfStory-en's curated Category > Chapter > Story > Part tree when
    // available.  The filename-derived list below remains a development
    // fallback for single-scene imports.
    json chapterTree;
    {
        const std::string candidates[] = { root + "/chapters.json", "assets/chapters.json",
                                            "chapters.json", "app0:/chapters.json" };
        for (const auto& candidate : candidates) {
            std::ifstream file(candidate);
            if (file) { file >> chapterTree; break; }
        }
    }
    if (chapterTree.contains("categories") && !chapterTree["categories"].empty()) {
        const auto& categories = chapterTree["categories"];
        while (true) {
            std::vector<std::string> categoryLabels;
            std::vector<std::vector<std::string>> categoryScenes;
            for (const auto& category : categories)
                categoryLabels.push_back(">  " + category.value("name", ""));
            for (const auto& category : categories) categoryScenes.push_back(sceneIds(category));
            int categoryIndex = p.pickScene(categoryLabels, "Story Selection", categoryScenes,
                                             {}, {}, "", "", false);
            if (categoryIndex < 0) break;
            const auto& category = categories[categoryIndex];
            while (true) {
                std::vector<std::string> chapterLabels;
                std::vector<std::vector<std::string>> chapterScenes;
                std::vector<std::string> chapterLogos, chapterWallpapers;
                for (const auto& chapter : category["chapters"]) {
                    chapterLabels.push_back(">  " + chapter.value("name", ""));
                    chapterScenes.push_back(sceneIds(chapter));
                    chapterLogos.push_back(chapter.value("logo", ""));
                    chapterWallpapers.push_back(chapter.value("wallpaper", ""));
                }
                int chapterIndex = p.pickScene(chapterLabels, category.value("name", "Story Selection"),
                                               chapterScenes, chapterLogos, chapterWallpapers);
                if (chapterIndex < 0) break;
                const auto& chapter = category["chapters"][chapterIndex];
                while (true) {
                    std::vector<std::string> storyLabels;
                    std::vector<std::vector<std::string>> storyScenes;
                    for (const auto& story : chapter["stories"]) {
                        std::string label = story.value("name", "");
                        if (story["files"].size() > 1) label = ">  " + label;
                        storyLabels.push_back(label);
                        storyScenes.push_back(sceneIds(story));
                    }
                    const std::string chapterLogo = chapter.value("logo", "");
                    const std::string chapterWallpaper = chapter.value("wallpaper", "");
                    int storyIndex = p.pickScene(storyLabels, chapter.value("name", "Chapter"), storyScenes,
                                                 {}, {}, chapterLogo, chapterWallpaper);
                    if (storyIndex < 0) break;
                    const auto& story = chapter["stories"][storyIndex];
                    int partIndex = 0;
                    if (story["files"].size() > 1) {
                        std::vector<std::string> partLabels;
                        std::vector<std::vector<std::string>> partScenes;
                        for (const auto& file : story["files"]) {
                            partLabels.push_back(file.value("label", "Part"));
                            partScenes.push_back(sceneIds(file));
                        }
                        partIndex = p.pickScene(partLabels, story.value("name", "Story"), partScenes,
                                                {}, {}, chapterLogo, chapterWallpaper);
                        if (partIndex < 0) continue;
                    }
                    const auto& file = story["files"][partIndex];
                    std::string id = file.value("id", "");
                    p.posLabel = chapter.value("name", "") + " \u00b7 " + story.value("name", "") +
                                 " \u00b7 " + file.value("label", "Part");
                    p.loadScene(root + "/scenes/" + id + ".ir.json");
                    if (p.run() == -1) return 0;
                }
            }
        }
        return 0;
    }
    while (true) {
        std::vector<std::string> chLabels;
        for (const auto& c : chapters) chLabels.push_back("Chapter " + c.label);
        int ch = chLabels.size() > 1 ? p.pickScene(chLabels, "Main Story") : 0;
        if (ch < 0) break;
        int st = chapters[ch].files.size() > 1
            ? p.pickScene(chapters[ch].labels, "Main Story \u00b7 Chapter " + chapters[ch].label) : 0;
        if (st < 0) continue;
        p.posLabel = "Chapter " + chapters[ch].label + " \u00b7 " + chapters[ch].labels[st];
        p.loadScene(root + "/scenes/" + chapters[ch].files[st]);
        if (p.run() == -1) break;   // -2 = back to chapter menu, 0 = scene finished
    }
    return 0;
}
