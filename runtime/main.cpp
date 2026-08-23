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
        manifest = loadJson(manifestPath());
        fontPath = findFont();
    }

    bool loadScene(const std::string& path) {
        scene = loadJson(path);
        // reset per-scene state
        stage.clear(); nightOn = false; blackAlpha = 0; bgId.clear();
        texCache.clear();
        return true;
    }

    void setup();
    bool isAuto() const { return autoAdvance; }
    int run();
    int pickScene(const std::vector<std::string>& names);

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
    bool autoAdvance = false;
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

void Player::setup() {
    CHECK(SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) == 0, "SDL_Init");
    CHECK((win = SDL_CreateWindow("gflvn", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                  SCREEN_W, SCREEN_H, 0)) != nullptr, "window");
    CHECK((ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC)) != nullptr ||
          (ren = SDL_CreateRenderer(win, -1, 0)) != nullptr, "renderer");
    CHECK(Mix_OpenAudio(44100, MIX_DEFAULT_FORMAT, 2, 2048) == 0, "mixer");
    CHECK(TTF_Init() == 0, "ttf");
    font = TTF_OpenFont(fontPath.c_str(), 26);
    nameFont = TTF_OpenFont(fontPath.c_str(), 22);
    CHECK(font && nameFont, "font load");
#ifdef GFLVN_VITA
    SDL_GameControllerOpen(0);   // vita buttons arrive as controller events
#endif
    autoAdvance = SDL_getenv("GFLVN_AUTO") != nullptr;
}

int Player::run() {

    const auto& events = scene["events"];
    size_t pc = 0;
    int sayPage = 0;
    const json* sayEv = nullptr;
    bool running = true;

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
#ifdef GFLVN_VITA
                    bool adv = e.type == SDL_CONTROLLERBUTTONDOWN;
#else
                    bool adv = false;
#endif
                    bool advKey = (e.type == SDL_KEYDOWN &&
                                (e.key.keysym.sym == SDLK_SPACE || e.key.keysym.sym == SDLK_RETURN ||
                                 e.key.keysym.sym == SDLK_x));
                    bool advTouch = (e.type == SDL_MOUSEBUTTONDOWN || e.type == SDL_FINGERDOWN);
                    adv = adv || advKey || advTouch;
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

int Player::pickScene(const std::vector<std::string>& names) {
    if (autoAdvance && !names.empty()) return 0;  // CI / smoke test
    int sel = 0;
    while (true) {
        SDL_SetRenderDrawColor(ren, 12, 14, 24, 255);
        SDL_RenderClear(ren);
        SDL_Color white{235, 235, 235, 255}, yellow{240, 200, 80, 255};
        for (size_t i = 0; i < names.size(); i++) {
            std::string label = (i == (size_t)sel ? "> " : "  ") + names[i];
            SDL_Surface* ts = TTF_RenderUTF8_Blended(font, label.c_str(), i == (size_t)sel ? yellow : white);
            SDL_Texture* tt = SDL_CreateTextureFromSurface(ren, ts);
            SDL_Rect r{60, 60 + (int)i * 38, ts->w, ts->h};
            SDL_RenderCopy(ren, tt, nullptr, &r);
            SDL_DestroyTexture(tt); SDL_FreeSurface(ts);
        }
        SDL_RenderPresent(ren);
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) return -1;
            if (e.type == SDL_KEYDOWN) {
                if (e.key.keysym.sym == SDLK_UP) sel = (sel + names.size() - 1) % names.size();
                if (e.key.keysym.sym == SDLK_DOWN) sel = (sel + 1) % names.size();
                if ((e.key.keysym.sym == SDLK_RETURN || e.key.keysym.sym == SDLK_x) && !names.empty()) return sel;
                if (e.key.keysym.sym == SDLK_ESCAPE) return -1;
            }
#ifdef GFLVN_VITA
            if (e.type == SDL_CONTROLLERBUTTONDOWN) {
                if (e.cbutton.button == SDL_CONTROLLER_BUTTON_DPAD_UP)
                    sel = (sel + names.size() - 1) % names.size();
                if (e.cbutton.button == SDL_CONTROLLER_BUTTON_DPAD_DOWN)
                    sel = (sel + 1) % names.size();
                if (e.cbutton.button == SDL_CONTROLLER_BUTTON_A && !names.empty()) return sel;
                if (e.cbutton.button == SDL_CONTROLLER_BUTTON_B) return -1;
            }
#endif
            if (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_LEFT) {
                int idx = (e.button.y - 60) / 38;
                if (idx >= 0 && idx < (int)names.size()) return idx;
            }
        }
        SDL_Delay(10);
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

int main(int argc, char** argv) {
#ifdef GFLVN_VITA
    // Vita launches without argv; data lives in the vpk
    std::string root = "app0:";
#else
    std::string root = argc > 1 ? argv[1] : "assets";
#endif
    Player p(root);

    // discover scenes: scenes/*.ir.json grouped into chapters, gfStory-en style:
    //   Chapter X-Y -> Part N (first/second). Fallback: single scene.ir.json.
    struct Chap { std::string label; std::vector<std::string> files; std::vector<std::string> labels; };
    std::vector<Chap> chapters;
    std::error_code ec;
    for (auto& f : fs::directory_iterator(root + "/scenes", ec)) {
        if (ec || f.path().extension() != ".json") continue;
        std::string fn = f.path().filename().string();
        if (fn.find(".ir.") == std::string::npos) continue;
        std::string id = f.path().stem().string();          // e.g. 1-2-2first
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
        return partKey(a.label) < partKey(b.label);
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
        for (const auto& c : chapters)
            for (const auto& fn : c.files) {
                p.loadScene(root + "/scenes/" + fn);
                p.run();
            }
        return 0;
    }
    while (true) {
        std::vector<std::string> chLabels;
        for (const auto& c : chapters) chLabels.push_back("Chapter " + c.label);
        int ch = chLabels.size() > 1 ? p.pickScene(chLabels) : 0;
        if (ch < 0) break;
        int st = chapters[ch].files.size() > 1 ? p.pickScene(chapters[ch].labels) : 0;
        if (st < 0) continue;
        p.loadScene(root + "/scenes/" + chapters[ch].files[st]);
        p.run();  // returns to menu when scene ends
    }
    return 0;
}
