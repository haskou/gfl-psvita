// gfl-psvita runtime: plays IR scene JSON (investigation §4 schema) with SDL2.
// Portable: builds for desktop and PS Vita from this single file.
#include <SDL.h>
#include <SDL_image.h>
#include <SDL_mixer.h>
#include <SDL_ttf.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <filesystem>
#include <sstream>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <vector>

using json = nlohmann::json;
namespace fs = std::filesystem;

static const int SCREEN_W = 960;
static const int SCREEN_H = 544;

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

struct Sprite {
    std::string name;   // char key
    std::string texKey;
    float x = 0, y = 0; // top-left after scaling
};

class Player {
public:
    Player(const std::string& rootDir) : root(rootDir) {
        manifest = loadJson(root + "/manifest.json");
        scene = loadJson(root + "/scene.ir.json");
        fontPath = findFont();
    }

    int run();

private:
    std::string root;
    std::string bgId;
    json manifest, scene;
    std::string fontPath;

    SDL_Window* win = nullptr;
    SDL_Renderer* ren = nullptr;
    TTF_Font* font = nullptr;
    TTF_Font* nameFont = nullptr;

    std::map<std::string, Tex> texCache;
    std::vector<Sprite> stage;
    bool nightOn = false;
    Uint8 blackAlpha = 0;   // persistent black overlay (black_on)
    Mix_Music* curMusic = nullptr;

    json loadJson(const std::string& p) {
        std::ifstream f(p);
        if (!f) { std::cerr << "cannot open " << p << "\n"; exit(1); }
        json j; f >> j; return j;
    }

    std::string findFont() {
        const std::string candidates[] = {
            root + "/../runtime/fonts/NotoSans-Regular.ttf",
            "runtime/fonts/NotoSans-Regular.ttf",
            "app0:/fonts/NotoSans-Regular.ttf",
            "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        };
        for (const auto& p : candidates)
            if (fs::exists(p)) return p;
        return "NotoSans-Regular.ttf";
    }

    std::string assetPath(const std::string& id) {
        // manifest.img / manifest.aud map key -> path relative to repo root
        std::string rel;
        if (manifest["img"].contains(id)) rel = manifest["img"][id].get<std::string>();
        else if (manifest["aud"].contains(id)) rel = manifest["aud"][id].get<std::string>();
        else return "";
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
    void drawAll(int sayPage, const json* sayEv);
    void playMusic(const std::string& id);
};

Tex* Player::getTex(const std::string& id) {
    auto it = texCache.find(id);
    if (it != texCache.end()) return &it->second;
    std::string path = assetPath(id);
    if (path.empty()) {
        std::cerr << "WARN: no asset for " << id << "\n";
        texCache[id] = {}; return &texCache[id];
    }
    SDL_Surface* surf = IMG_Load(path.c_str());
    if (!surf) { std::cerr << "WARN: IMG_Load " << path << ": " << IMG_GetError() << "\n"; texCache[id] = {}; return &texCache[id]; }
    Tex t;
    t.tex = SDL_CreateTextureFromSurface(ren, surf);
    t.w = surf->w; t.h = surf->h;
    SDL_FreeSurface(surf);
    texCache[id] = t;
    return &texCache[id];
}

// assign slots left->right by stage order; scale sprite to ~92% screen height
void Player::layoutStage() {
    int n = (int)stage.size();
    for (int i = 0; i < n; i++) {
        Tex* t = getTex(stage[i].texKey);
        if (!t->tex) continue;
        float scale = (SCREEN_H * 0.92f) / t->h;
        int w = (int)(t->w * scale), h = (int)(t->h * scale);
        int slotW = SCREEN_W / n;
        stage[i].x = slotW * i + (slotW - w) / 2.0f;
        stage[i].y = SCREEN_H - h - 40;
    }
}

void Player::playMusic(const std::string& id) {
    std::string path = assetPath(id);
    if (path.empty()) return;
    if (curMusic) { Mix_HaltMusic(); Mix_FreeMusic(curMusic); curMusic = nullptr; }
    curMusic = Mix_LoadMUS(path.c_str());
    if (curMusic) Mix_PlayMusic(curMusic, -1);
    else std::cerr << "WARN: music " << path << ": " << Mix_GetError() << "\n";
}

void Player::drawAll(int sayPage, const json* sayEv) {
    // background
    SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
    SDL_RenderClear(ren);
    if (!bgId.empty()) {
        Tex* bg = getTex(bgId);
        if (bg->tex) {
            double s = std::max((double)SCREEN_W / bg->w, (double)SCREEN_H / bg->h);
            int w = (int)(bg->w * s), h = (int)(bg->h * s);
            SDL_Rect r{ (SCREEN_W - w) / 2, (SCREEN_H - h) / 2, w, h };
            SDL_RenderCopy(ren, bg->tex, nullptr, &r);
        }
    }
    // night tint
    if (nightOn) {
        SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(ren, 16, 24, 80, 90);
        SDL_RenderFillRect(ren, nullptr);
    }
    // sprites back-to-front
    for (auto& sp : stage) {
        Tex* t = getTex(sp.texKey);
        if (!t->tex) continue;
        SDL_Rect r{ (int)sp.x, (int)sp.y, 0, 0 };
        SDL_QueryTexture(t->tex, nullptr, nullptr, &r.w, &r.h);
        float scale = (SCREEN_H * 0.92f) / r.h;
        r.w = (int)(r.w * scale); r.h = (int)(r.h * scale);
        r.x = (int)sp.x; r.y = SCREEN_H - r.h - 40;
        SDL_RenderCopy(ren, t->tex, nullptr, &r);
    }
    // persistent black
    if (blackAlpha > 0) {
        SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(ren, 0, 0, 0, blackAlpha);
        SDL_RenderFillRect(ren, nullptr);
    }
    // textbox
    if (sayEv && sayEv != (const json*)1) {
        const auto& ev = *sayEv;
        std::string name = ev.value("name", "");
        const auto& pages = ev["text"];
        int page = std::min(sayPage, (int)pages.size() - 1);
        std::string text = pages[page].get<std::string>();

        SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(ren, 8, 10, 14, 210);
        SDL_Rect box{ 20, SCREEN_H - 150, SCREEN_W - 40, 130 };
        SDL_RenderFillRect(ren, &box);
        SDL_SetRenderDrawColor(ren, 200, 200, 200, 160);
        SDL_RenderDrawRect(ren, &box);

        SDL_Color white{ 235, 235, 235, 255 };
        if (!name.empty()) {
            SDL_Surface* ns = TTF_RenderUTF8_Blended(nameFont, name.c_str(), SDL_Color{ 120, 190, 255, 255 });
            SDL_Texture* nt = SDL_CreateTextureFromSurface(ren, ns);
            SDL_Rect nr{ box.x + 16, box.y - 18, ns->w, ns->h };
            SDL_RenderCopy(ren, nt, nullptr, &nr);
            SDL_DestroyTexture(nt); SDL_FreeSurface(ns);
        }
        // word-wrap to box width
        std::vector<std::string> lines;
        int maxW = box.w - 32;
        std::string cur;
        std::istringstream iss(text);
        std::string word;
        while (iss >> word) {
            std::string trial = cur.empty() ? word : cur + " " + word;
            int tw = 0;
            TTF_SizeUTF8(font, trial.c_str(), &tw, nullptr);
            if (tw > maxW && !cur.empty()) { lines.push_back(cur); cur = word; }
            else cur = trial;
        }
        lines.push_back(cur);
        int y = box.y + 14;
        for (auto& ln : lines) {
            SDL_Surface* ts = TTF_RenderUTF8_Blended(font, ln.c_str(), white);
            SDL_Texture* tt2 = SDL_CreateTextureFromSurface(ren, ts);
            SDL_Rect tr{ box.x + 16, y, ts->w, ts->h };
            SDL_RenderCopy(ren, tt2, nullptr, &tr);
            SDL_DestroyTexture(tt2); SDL_FreeSurface(ts);
            y += ts->h + 6;
        }
        // advance indicator
        SDL_SetRenderDrawColor(ren, 235, 235, 235, 255);
        SDL_Rect tri{ box.x + box.w - 26, box.y + box.h - 18, 12, 6 };
        SDL_RenderFillRect(ren, &tri);
    }
    SDL_RenderPresent(ren);
}

int Player::run() {
    CHECK(SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) == 0, "SDL_Init");
    CHECK((win = SDL_CreateWindow("gflvn", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                  SCREEN_W, SCREEN_H, 0)) != nullptr, "window");
    CHECK((ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC)) != nullptr ||
          (ren = SDL_CreateRenderer(win, -1, 0)) != nullptr, "renderer");
    CHECK(Mix_OpenAudio(44100, MIX_DEFAULT_FORMAT, 2, 2048) == 0, "mixer");
    CHECK(TTF_Init() == 0, "ttf");
    font = TTF_OpenFont(fontPath.c_str(), 26);
    nameFont = TTF_OpenFont(fontPath.c_str(), 22);
    CHECK(font && nameFont, "font load");

    drawAll(0, nullptr);  // clear screen before first event

    const auto& events = scene["events"];
    size_t pc = 0;
    int sayPage = 0;
    const json* sayEv = nullptr;
    bool running = true;
    bool autoAdvance = SDL_getenv("GFLVN_AUTO") != nullptr;

    while (running && pc < events.size()) {
        const auto& ev = events[pc];
        std::string t = ev.value("t", "");
        if (t == "start") { pc++; continue; }
        if (t == "end") break;

        if (t == "bg") {
            bgId = ev["id"];
            if (ev.value("transition", "") == "fade_black") {
                // quick fade-in-from-black approximation
                blackAlpha = 255;
                drawAll(sayPage, sayEv);
                for (int a = 255; a >= 0; a -= 24) { blackAlpha = a; drawAll(sayPage, sayEv); SDL_Delay(16); }
                blackAlpha = 0;
            }
        } else if (t == "music") {
            playMusic(ev["id"]);
        } else if (t == "sfx") {
            std::string path = assetPath(ev["id"]);
            if (!path.empty()) {
                Mix_Chunk* c = Mix_LoadWAV(path.c_str());
                if (c) Mix_PlayChannel(-1, c, 0);  // ponytail: leaked chunk, SE library is small
                else std::cerr << "WARN sfx " << path << ": " << Mix_GetError() << "\n";
            }
        } else if (t == "show") {
            Sprite sp; sp.name = ev["char"]; sp.texKey = "spr_" + ev["char"].get<std::string>() + "_" + std::to_string(ev["expr"].get<int>());
            stage.erase(std::remove_if(stage.begin(), stage.end(),
                        [&](const Sprite& s){ return s.name == sp.name; }), stage.end());
            stage.push_back(sp);
            layoutStage();
        } else if (t == "hide") {
            stage.erase(std::remove_if(stage.begin(), stage.end(),
                        [&](const Sprite& s){ return s.name == ev["char"].get<std::string>(); }), stage.end());
            layoutStage();
        } else if (t == "night") {
            nightOn = ev.value("on", false);
        } else if (t == "effect") {
            std::string k = ev.value("kind", "");
            if (k == "black_on") blackAlpha = 255;
            else if (k == "fade_from_black") {
                for (int a = 255; a >= 0; a -= 16) { blackAlpha = a; drawAll(sayPage, sayEv); SDL_Delay(16); }
                blackAlpha = 0;
            }
            // shake/flash/eyes_open/memory masks: logged no-op for now
        } else if (t == "say") {
            sayEv = &ev; sayPage = 0;
        } else if (t == "choice") {
            // no choices in the MVP scene; step 05 handles them
        }

        drawAll(sayPage, sayEv);

        if (t == "say") {
            // wait for advance through all pages of this line
            bool lineDone = false;
            while (!lineDone && running) {
                if (autoAdvance) break;  // CI / smoke test
                SDL_Event e;
                while (SDL_PollEvent(&e)) {
                    if (e.type == SDL_QUIT) running = false;
                    if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_ESCAPE) running = false;
                    bool adv = (e.type == SDL_KEYDOWN &&
                                (e.key.keysym.sym == SDLK_SPACE || e.key.keysym.sym == SDLK_RETURN ||
                                 e.key.keysym.sym == SDLK_x))
                               || (e.type == SDL_MOUSEBUTTONDOWN || e.type == SDL_FINGERDOWN);
                    if (adv) {
                        sayPage++;
                        if (sayPage >= (int)(*sayEv)["text"].size()) lineDone = true;
                        drawAll(sayPage, sayEv);
                    }
                }
                SDL_Delay(10);
            }
            sayEv = nullptr;
        }
        pc++;
    }
    std::cout << "scene finished\n";
    return 0;
}

int main(int argc, char** argv) {
    std::string root = argc > 1 ? argv[1] : "assets";
    Player p(root);
    return p.run();
}
