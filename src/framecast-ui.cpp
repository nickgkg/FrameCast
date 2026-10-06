// Copyright 2026 Nicholas Guetter. MIT License, see LICENSE.

// framecast-ui: SteamVR dashboard panel "Cast to TV" for FrameCast.
//
// Shows the cast state, lists the Chromecasts on the current network (go-chromecast ls) and starts or
// stops the cast through framecast.sh. framecast.sh writes its state to $XDG_RUNTIME_DIR/framecast.state.
//
// Overlay images are drawn with Cairo and given to SteamVR as Vulkan textures. (SetOverlayFromFile caches
// each path forever, so it cannot show changing content.)
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <regex>
#include <spawn.h>
#include <string>
#include <thread>
#include <vector>
#include <dirent.h>
#include <sys/wait.h>
#include <unistd.h>
#include <cairo/cairo.h>
#include <vulkan/vulkan.h>
#include "openvr.h"

extern char **environ;

#define LOG(...) fprintf(stderr, "framecast-ui: " __VA_ARGS__)

using Clock = std::chrono::steady_clock;

static const int W = 1000, H = 800;
static std::string g_home, g_runDir, g_script, g_goChromecast, g_lastDeviceFile, g_stabFile;

// ---------------------------------------------------------------------------------------------------------
// Helpers

static std::string readFile(const std::string &path) {
    std::ifstream f(path);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

static void writeFile(const std::string &path, const std::string &data) {
    std::ofstream f(path);
    f << data;
}

static std::string trim(std::string s) {
    while (!s.empty() && isspace((unsigned char)s.back())) s.pop_back();
    size_t i = 0;
    while (i < s.size() && isspace((unsigned char)s[i])) i++;
    return s.substr(i);
}

// Run a command and do not wait for it (the child is reaped by SIGCHLD = SIG_IGN).
static void spawnDetached(const std::vector<std::string> &args) {
    std::vector<char *> argv;
    for (auto &a : args) argv.push_back(const_cast<char *>(a.c_str()));
    argv.push_back(nullptr);
    pid_t pid;
    if (posix_spawn(&pid, argv[0], nullptr, nullptr, argv.data(), environ) != 0) LOG("could not run %s\n", argv[0]);
}

static std::vector<std::string> splitWords(const char *s) {
    std::vector<std::string> v;
    std::string cur;
    for (; *s; s++) {
        if (*s == ' ') { if (!cur.empty()) v.push_back(cur); cur.clear(); }
        else cur += *s;
    }
    if (!cur.empty()) v.push_back(cur);
    return v;
}

static bool processRunning(const char *name) {
    DIR *d = opendir("/proc");
    if (!d) return false;
    bool found = false;
    while (dirent *e = readdir(d)) {
        if (!isdigit((unsigned char)e->d_name[0])) continue;
        std::string comm = trim(readFile(std::string("/proc/") + e->d_name + "/comm"));
        if (comm == name) { found = true; break; }
    }
    closedir(d);
    return found;
}

// ---------------------------------------------------------------------------------------------------------
// Cast state (from framecast.sh) and device discovery

struct Device { std::string name, model; };

struct CastState {
    std::string state = "idle"; // idle | starting | casting | error
    std::string device, message;
    bool operator==(const CastState &o) const { return state == o.state && device == o.device && message == o.message; }
};

static CastState readCastState() {
    CastState cs;
    std::map<std::string, std::string> kv;
    std::string data = readFile(g_runDir + "/framecast.state");
    size_t pos = 0;
    while (pos < data.size()) {
        size_t nl = data.find('\n', pos);
        std::string line = data.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
        size_t eq = line.find('=');
        if (eq != std::string::npos) kv[line.substr(0, eq)] = line.substr(eq + 1);
        if (nl == std::string::npos) break;
        pos = nl + 1;
    }
    if (kv.count("state")) cs.state = kv["state"];
    cs.device = kv["device"];
    cs.message = kv["message"];
    // A crashed cast leaves "casting" behind; trust the pid.
    if (cs.state == "starting" || cs.state == "casting") {
        int pid = atoi(kv["pid"].c_str());
        if (pid <= 0 || kill(pid, 0) != 0) { cs.state = "error"; cs.message = "The cast stopped unexpectedly"; }
    }
    return cs;
}

class Discovery {
public:
    void start() {
        if (busy_.exchange(true)) return;
        std::thread([this] {
            std::vector<Device> found;
            std::string cmd = "'" + g_goChromecast + "' ls --dns-timeout 4 2>/dev/null";
            if (FILE *p = popen(cmd.c_str(), "r")) {
                static const std::regex re(R"re(device="([^"]*)" device_name="([^"]*)")re");
                char line[1024];
                while (fgets(line, sizeof line, p)) {
                    std::cmatch m;
                    if (std::regex_search(line, m, re)) found.push_back({m[2].str(), m[1].str()});
                }
                pclose(p);
            }
            {
                std::lock_guard<std::mutex> l(mu_);
                devices_ = found;
                searched_ = true;
                last_ = Clock::now();
            }
            LOG("found %zu cast devices\n", found.size());
            busy_ = false;
            changed_ = true;
        }).detach();
    }
    bool busy() const { return busy_; }
    bool searched() { std::lock_guard<std::mutex> l(mu_); return searched_; }
    std::vector<Device> devices() { std::lock_guard<std::mutex> l(mu_); return devices_; }
    double age() { std::lock_guard<std::mutex> l(mu_); return searched_ ? std::chrono::duration<double>(Clock::now() - last_).count() : 1e9; }
    bool takeChanged() { return changed_.exchange(false); }

private:
    std::atomic<bool> busy_{false}, changed_{false};
    std::mutex mu_;
    std::vector<Device> devices_;
    bool searched_ = false;
    Clock::time_point last_;
};

// ---------------------------------------------------------------------------------------------------------
// Drawing

struct Rect {
    double x, y, w, h;
    bool contains(double px, double py) const { return px >= x && px < x + w && py >= y && py < y + h; }
};

struct Button { std::string id; Rect r; };

static void roundRect(cairo_t *c, Rect r, double rad) {
    cairo_new_sub_path(c);
    cairo_arc(c, r.x + r.w - rad, r.y + rad, rad, -M_PI / 2, 0);
    cairo_arc(c, r.x + r.w - rad, r.y + r.h - rad, rad, 0, M_PI / 2);
    cairo_arc(c, r.x + rad, r.y + r.h - rad, rad, M_PI / 2, M_PI);
    cairo_arc(c, r.x + rad, r.y + rad, rad, M_PI, 3 * M_PI / 2);
    cairo_close_path(c);
}

static void rgb(cairo_t *c, unsigned hex, double a = 1.0) {
    cairo_set_source_rgba(c, ((hex >> 16) & 255) / 255.0, ((hex >> 8) & 255) / 255.0, (hex & 255) / 255.0, a);
}

static void font(cairo_t *c, double size, bool bold) {
    cairo_select_font_face(c, "Noto Sans", CAIRO_FONT_SLANT_NORMAL, bold ? CAIRO_FONT_WEIGHT_BOLD : CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(c, size);
}

static double textWidth(cairo_t *c, const std::string &s) {
    cairo_text_extents_t e;
    cairo_text_extents(c, s.c_str(), &e);
    return e.x_advance;
}

// Draw text, cut with "…" to fit maxW. align: 0 = left, 1 = right, 0.5 = center.
static void text(cairo_t *c, std::string s, double x, double y, double maxW = 1e9, double align = 0) {
    if (textWidth(c, s) > maxW) {
        while (!s.empty() && textWidth(c, s + "…") > maxW) {
            s.pop_back();
            while (!s.empty() && (s.back() & 0xC0) == 0x80) s.pop_back(); // Keep UTF-8 whole.
        }
        s += "…";
    }
    cairo_move_to(c, x - textWidth(c, s) * align, y);
    cairo_show_text(c, s.c_str());
}

// Small TV-with-cast-waves icon.
static void castIcon(cairo_t *c, double x, double y, double s, unsigned color) {
    rgb(c, color);
    cairo_set_line_width(c, s * 0.08);
    cairo_set_line_cap(c, CAIRO_LINE_CAP_ROUND);
    cairo_move_to(c, x + s * 0.1, y + s * 0.35);
    cairo_line_to(c, x + s * 0.1, y + s * 0.15);
    cairo_line_to(c, x + s * 0.9, y + s * 0.15);
    cairo_line_to(c, x + s * 0.9, y + s * 0.85);
    cairo_line_to(c, x + s * 0.6, y + s * 0.85);
    cairo_stroke(c);
    for (int i = 1; i <= 2; i++) {
        cairo_arc(c, x + s * 0.1, y + s * 0.85, s * 0.2 * i, -M_PI / 2, 0);
        cairo_stroke(c);
    }
    cairo_arc(c, x + s * 0.1, y + s * 0.85, s * 0.06, 0, 2 * M_PI);
    cairo_fill(c);
}

struct View {
    CastState cast;
    bool pending = false; // A start or stop was clicked; framecast.sh has not written a new state yet.
    std::string pendingDevice;
    bool searching = false, searched = false;
    std::vector<Device> devices;
    std::string lastDevice;
    bool level = false; // Keep the horizon level.
    std::string hover;
};

static const unsigned BG = 0x15181e, PANEL = 0x232833, PANEL_HI = 0x2e3543, TEXT = 0xf2f4f8, DIM = 0x9aa3b2,
                      BLUE = 0x3d8bfd, GREEN = 0x37c871, YELLOW = 0xf2b33d, RED = 0xf0554a;

static std::vector<Button> drawPanel(const View &v, cairo_surface_t *s) {
    std::vector<Button> buttons;
    cairo_t *c = cairo_create(s);
    rgb(c, BG);
    cairo_paint(c);

    // Header.
    castIcon(c, 40, 34, 56, TEXT);
    rgb(c, TEXT);
    font(c, 46, true);
    text(c, "Cast to TV", 116, 82);

    // Status line.
    std::string status;
    unsigned dot;
    bool active = false;
    if (v.pending) {
        status = v.pendingDevice.empty() ? "Stopping…" : "Starting cast to " + v.pendingDevice + "…";
        dot = YELLOW;
        active = !v.pendingDevice.empty();
    } else if (v.cast.state == "casting") {
        status = "Casting to " + v.cast.device;
        dot = GREEN;
        active = true;
    } else if (v.cast.state == "starting") {
        status = "Starting cast to " + v.cast.device + "…";
        dot = YELLOW;
        active = true;
    } else if (v.cast.state == "error") {
        status = v.cast.message.empty() ? "The cast failed" : v.cast.message;
        dot = RED;
    } else {
        status = "Not casting";
        dot = DIM;
    }
    rgb(c, dot);
    cairo_arc(c, 54, 140, 11, 0, 2 * M_PI);
    cairo_fill(c);
    rgb(c, TEXT);
    font(c, 32, false);
    text(c, status, 80, 152, active ? 600 : 880);

    if (active) {
        Rect r{730, 106, 230, 66};
        buttons.push_back({"stop", r});
        roundRect(c, r, 14);
        rgb(c, v.hover == "stop" ? 0xff7468 : RED);
        cairo_fill(c);
        rgb(c, 0xffffff);
        font(c, 30, true);
        text(c, "Stop", r.x + r.w / 2, r.y + 44, r.w, 0.5);
    }

    // Horizon option (applies at once, also during a cast).
    {
        Rect r{40, 196, 920, 78};
        buttons.push_back({"level", r});
        roundRect(c, r, 14);
        rgb(c, v.hover == "level" ? PANEL_HI : PANEL);
        cairo_fill(c);
        rgb(c, TEXT);
        font(c, 30, true);
        text(c, "Keep the horizon level", r.x + 26, r.y + 36, 640);
        rgb(c, DIM);
        font(c, 22, false);
        text(c, v.level ? "The TV picture turns against your head tilt" : "The TV picture tilts with your head",
             r.x + 26, r.y + 64, 640);
        Rect sw{r.x + r.w - 26 - 104, r.y + 17, 104, 44};
        roundRect(c, sw, 22);
        rgb(c, v.level ? GREEN : 0x4a5262);
        cairo_fill(c);
        rgb(c, 0xffffff);
        cairo_arc(c, v.level ? sw.x + sw.w - 22 : sw.x + 22, sw.y + 22, 17, 0, 2 * M_PI);
        cairo_fill(c);
    }

    // Device list.
    rgb(c, DIM);
    font(c, 26, false);
    text(c, "Chromecasts on this network", 40, 336);
    {
        Rect r{770, 296, 190, 56};
        buttons.push_back({"refresh", r});
        roundRect(c, r, 12);
        rgb(c, v.hover == "refresh" ? PANEL_HI : PANEL);
        cairo_fill(c);
        rgb(c, v.searching ? DIM : TEXT);
        font(c, 26, false);
        text(c, v.searching ? "Searching…" : "Refresh", r.x + r.w / 2, r.y + 37, r.w - 10, 0.5);
    }

    const int maxRows = 4;
    if (v.devices.empty()) {
        rgb(c, DIM);
        font(c, 28, false);
        if (v.searching || !v.searched) {
            text(c, "Searching for Chromecasts…", 40, 430);
        } else {
            text(c, "No Chromecast found.", 40, 430);
            text(c, "The Frame and the TV must use the same Wi-Fi network.", 40, 475, 920);
        }
    }
    for (size_t i = 0; i < v.devices.size() && (int)i < maxRows; i++) {
        const Device &d = v.devices[i];
        Rect r{40, 368 + i * 92.0, 920, 78};
        std::string id = "dev:" + d.name;
        bool current = active && (v.pending ? v.pendingDevice : v.cast.device) == d.name;
        buttons.push_back({id, r});
        roundRect(c, r, 14);
        rgb(c, v.hover == id ? PANEL_HI : PANEL);
        cairo_fill(c);
        if (current) {
            roundRect(c, r, 14);
            rgb(c, v.cast.state == "casting" && !v.pending ? GREEN : YELLOW);
            cairo_set_line_width(c, 3);
            cairo_stroke(c);
        }
        castIcon(c, r.x + 22, r.y + 19, 40, current ? GREEN : DIM);
        rgb(c, TEXT);
        font(c, 32, true);
        text(c, d.name, r.x + 84, r.y + 38, 600);
        rgb(c, DIM);
        font(c, 22, false);
        std::string sub = d.model;
        if (d.name == v.lastDevice) sub += "  ·  used last";
        text(c, sub, r.x + 84, r.y + 66, 600);
        rgb(c, current ? GREEN : BLUE);
        font(c, 28, true);
        text(c, current ? (v.cast.state == "casting" && !v.pending ? "Casting" : "Starting…") : "Cast here",
             r.x + r.w - 26, r.y + 49, 220, 1);
    }
    if ((int)v.devices.size() > maxRows) {
        rgb(c, DIM);
        font(c, 22, false);
        text(c, "+ " + std::to_string(v.devices.size() - maxRows) + " more not shown", 40, 368 + maxRows * 92.0 + 6);
    }

    // Footer.
    rgb(c, DIM);
    font(c, 22, false);
    text(c, "The TV shows the picture and plays the sound about 0.3 seconds late.", 40, H - 28, 920);

    cairo_destroy(c);
    return buttons;
}

static const int THUMB = 256;

static void drawThumb(bool casting, cairo_surface_t *s) {
    cairo_t *c = cairo_create(s);
    cairo_set_operator(c, CAIRO_OPERATOR_CLEAR);
    cairo_paint(c);
    cairo_set_operator(c, CAIRO_OPERATOR_OVER);
    castIcon(c, 28, 28, 200, casting ? GREEN : TEXT);
    cairo_destroy(c);
}

// ---------------------------------------------------------------------------------------------------------
// Vulkan textures for the overlays

#define VK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { LOG("%s failed: %d\n", #x, r_); exit(1); } } while (0)

class Gpu {
public:
    void init() {
        char buf[4096];
        vr::VRCompositor()->GetVulkanInstanceExtensionsRequired(buf, sizeof buf);
        auto iext = splitWords(buf);
        std::vector<const char *> iextp;
        for (auto &e : iext) iextp.push_back(e.c_str());
        VkApplicationInfo ai{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        ai.pApplicationName = "framecast-ui";
        ai.apiVersion = VK_API_VERSION_1_1;
        VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        ici.pApplicationInfo = &ai;
        ici.enabledExtensionCount = iextp.size();
        ici.ppEnabledExtensionNames = iextp.data();
        VK(vkCreateInstance(&ici, nullptr, &inst));

        uint64_t phys = 0;
        vr::VRSystem()->GetOutputDevice(&phys, vr::TextureType_Vulkan, inst);
        if (phys) pd = (VkPhysicalDevice)phys;
        else { uint32_t n = 1; vkEnumeratePhysicalDevices(inst, &n, &pd); }
        vkGetPhysicalDeviceMemoryProperties(pd, &mp);

        vr::VRCompositor()->GetVulkanDeviceExtensionsRequired(pd, buf, sizeof buf);
        auto dext = splitWords(buf);
        std::vector<const char *> dextp;
        for (auto &e : dext) dextp.push_back(e.c_str());
        uint32_t nqf = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(pd, &nqf, nullptr);
        std::vector<VkQueueFamilyProperties> qfs(nqf);
        vkGetPhysicalDeviceQueueFamilyProperties(pd, &nqf, qfs.data());
        for (uint32_t i = 0; i < nqf; i++) if (qfs[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { qfi = i; break; }
        float prio = 0.0f;
        VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        qci.queueFamilyIndex = qfi; qci.queueCount = 1; qci.pQueuePriorities = &prio;
        VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        dci.queueCreateInfoCount = 1; dci.pQueueCreateInfos = &qci;
        dci.enabledExtensionCount = dextp.size(); dci.ppEnabledExtensionNames = dextp.data();
        VK(vkCreateDevice(pd, &dci, nullptr, &dev));
        vkGetDeviceQueue(dev, qfi, 0, &q);

        VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pci.queueFamilyIndex = qfi; pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        VK(vkCreateCommandPool(dev, &pci, nullptr, &pool));
        VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        cai.commandPool = pool; cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cai.commandBufferCount = 1;
        VK(vkAllocateCommandBuffers(dev, &cai, &cb));
        VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        VK(vkCreateFence(dev, &fci, nullptr, &fence));
    }

    uint32_t memType(uint32_t bits, VkMemoryPropertyFlags want) const {
        for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
            if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) return i;
        LOG("no memory type for flags 0x%x\n", want);
        exit(1);
    }

    VkInstance inst; VkPhysicalDevice pd; VkDevice dev; VkQueue q; uint32_t qfi = 0;
    VkPhysicalDeviceMemoryProperties mp;
    VkCommandPool pool; VkCommandBuffer cb; VkFence fence;
};

// One overlay's texture. Two images alternate so SteamVR never reads an image while it is rewritten.
class OverlayTexture {
public:
    void init(Gpu *gpu, uint32_t w, uint32_t h) {
        g_ = gpu; w_ = w; h_ = h;
        for (auto &img : imgs_) {
            VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
            ici.imageType = VK_IMAGE_TYPE_2D; ici.format = VK_FORMAT_B8G8R8A8_UNORM; ici.extent = {w, h, 1};
            ici.mipLevels = 1; ici.arrayLayers = 1; ici.samples = VK_SAMPLE_COUNT_1_BIT;
            ici.tiling = VK_IMAGE_TILING_OPTIMAL;
            ici.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
            ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            VK(vkCreateImage(g_->dev, &ici, nullptr, &img.image));
            VkMemoryRequirements mr;
            vkGetImageMemoryRequirements(g_->dev, img.image, &mr);
            VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
            mai.allocationSize = mr.size; mai.memoryTypeIndex = g_->memType(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            VK(vkAllocateMemory(g_->dev, &mai, nullptr, &img.mem));
            vkBindImageMemory(g_->dev, img.image, img.mem, 0);
        }
        VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bci.size = (VkDeviceSize)w * h * 4; bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        VK(vkCreateBuffer(g_->dev, &bci, nullptr, &staging_));
        VkMemoryRequirements mr;
        vkGetBufferMemoryRequirements(g_->dev, staging_, &mr);
        VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        mai.allocationSize = mr.size;
        mai.memoryTypeIndex = g_->memType(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        VK(vkAllocateMemory(g_->dev, &mai, nullptr, &stagingMem_));
        vkBindBufferMemory(g_->dev, staging_, stagingMem_, 0);
        VK(vkMapMemory(g_->dev, stagingMem_, 0, VK_WHOLE_SIZE, 0, (void **)&mapped_));
    }

    // Upload a Cairo ARGB32 surface (BGRA bytes, premultiplied) and set it on the overlay.
    void upload(cairo_surface_t *s, vr::VROverlayHandle_t ov) {
        cairo_surface_flush(s);
        const unsigned char *src = cairo_image_surface_get_data(s);
        int stride = cairo_image_surface_get_stride(s);
        for (uint32_t y = 0; y < h_; y++) memcpy(mapped_ + (size_t)y * w_ * 4, src + (size_t)y * stride, (size_t)w_ * 4);

        Img &img = imgs_[cur_ ^= 1];
        VK(vkResetCommandBuffer(g_->cb, 0));
        VkCommandBufferBeginInfo cbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        cbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(g_->cb, &cbi);
        VkImageMemoryBarrier bar{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        bar.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; bar.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        bar.srcQueueFamilyIndex = bar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bar.image = img.image; bar.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        bar.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(g_->cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &bar);
        VkBufferImageCopy reg{};
        reg.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        reg.imageExtent = {w_, h_, 1};
        vkCmdCopyBufferToImage(g_->cb, staging_, img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &reg);
        // SteamVR expects Vulkan textures in TRANSFER_SRC_OPTIMAL.
        bar.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; bar.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        bar.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; bar.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        vkCmdPipelineBarrier(g_->cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &bar);
        vkEndCommandBuffer(g_->cb);
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.commandBufferCount = 1; si.pCommandBuffers = &g_->cb;
        VK(vkQueueSubmit(g_->q, 1, &si, g_->fence));
        VK(vkWaitForFences(g_->dev, 1, &g_->fence, VK_TRUE, UINT64_MAX));
        vkResetFences(g_->dev, 1, &g_->fence);

        vr::VRVulkanTextureData_t vt{};
        vt.m_nImage = (uint64_t)img.image;
        vt.m_pDevice = g_->dev; vt.m_pPhysicalDevice = g_->pd; vt.m_pInstance = g_->inst;
        vt.m_pQueue = g_->q; vt.m_nQueueFamilyIndex = g_->qfi;
        vt.m_nWidth = w_; vt.m_nHeight = h_; vt.m_nFormat = VK_FORMAT_B8G8R8A8_UNORM; vt.m_nSampleCount = 1;
        vr::Texture_t tex{&vt, vr::TextureType_Vulkan, vr::ColorSpace_Auto};
        vr::EVROverlayError e = vr::VROverlay()->SetOverlayTexture(ov, &tex);
        if (e != vr::VROverlayError_None) LOG("SetOverlayTexture failed: %s\n", vr::VROverlay()->GetOverlayErrorNameFromEnum(e));
    }

private:
    struct Img { VkImage image; VkDeviceMemory mem; };
    Gpu *g_ = nullptr;
    uint32_t w_ = 0, h_ = 0;
    Img imgs_[2];
    int cur_ = 0;
    VkBuffer staging_; VkDeviceMemory stagingMem_; unsigned char *mapped_ = nullptr;
};

// ---------------------------------------------------------------------------------------------------------

static volatile sig_atomic_t g_stop = 0;

int main() {
    signal(SIGCHLD, SIG_IGN);
    signal(SIGINT, [](int) { g_stop = 1; });
    signal(SIGTERM, [](int) { g_stop = 1; });

    const char *home = getenv("HOME");
    g_home = home ? home : "/home/steamos";
    const char *rd = getenv("XDG_RUNTIME_DIR");
    g_runDir = rd ? rd : "/run/user/" + std::to_string(getuid());
    char self[4096];
    ssize_t n = readlink("/proc/self/exe", self, sizeof self - 1);
    std::string base = n > 0 ? std::string(self, n) : g_home + "/FrameCast/bin/framecast-ui";
    base = base.substr(0, base.rfind('/'));          // .../FrameCast/bin
    base = base.substr(0, base.rfind('/'));          // .../FrameCast
    g_script = base + "/framecast.sh";
    g_goChromecast = base + "/bin/go-chromecast";
    g_lastDeviceFile = base + "/last_device";
    g_stabFile = base + "/stabilize";

    // Do not start SteamVR ourselves; wait until it runs.
    while (!g_stop && !processRunning("vrcompositor")) sleep(2);
    if (g_stop) return 0;

    vr::EVRInitError ierr = vr::VRInitError_None;
    vr::VR_Init(&ierr, vr::VRApplication_Overlay);
    if (ierr != vr::VRInitError_None) {
        LOG("VR_Init failed: %s\n", vr::VR_GetVRInitErrorAsEnglishDescription(ierr));
        return 1;
    }

    vr::VROverlayHandle_t ov, thumb;
    vr::EVROverlayError oerr = vr::VROverlay()->CreateDashboardOverlay("framecast.panel", "Cast to TV", &ov, &thumb);
    if (oerr != vr::VROverlayError_None) {
        LOG("CreateDashboardOverlay failed: %s\n", vr::VROverlay()->GetOverlayErrorNameFromEnum(oerr));
        return 1;
    }
    vr::VROverlay()->SetOverlayWidthInMeters(ov, 2.0f);
    vr::VROverlay()->SetOverlayInputMethod(ov, vr::VROverlayInputMethod_Mouse);
    vr::HmdVector2_t mouseScale{(float)W, (float)H};
    vr::VROverlay()->SetOverlayMouseScale(ov, &mouseScale);
    LOG("dashboard panel ready\n");

    View v;
    v.lastDevice = trim(readFile(g_lastDeviceFile));
    v.level = trim(readFile(g_stabFile)) == "1";
    v.cast = readCastState();
    Discovery disc;
    disc.start();

    Gpu gpu;
    gpu.init();
    OverlayTexture panelTex, thumbTex;
    panelTex.init(&gpu, W, H);
    thumbTex.init(&gpu, THUMB, THUMB);
    cairo_surface_t *panelSurf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, W, H);
    cairo_surface_t *thumbSurf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, THUMB, THUMB);

    std::vector<Button> buttons;
    bool dirty = true, thumbCasting = false, thumbDrawn = false, visible = false;
    Clock::time_point pendingSince;
    CastState notified = v.cast;

    auto notify = [&](const std::string &msg) {
        vr::VRNotificationId id;
        vr::EVRNotificationError e = vr::VRNotifications()->CreateNotification(
            ov, 0, vr::EVRNotificationType_Transient, msg.c_str(), vr::EVRNotificationStyle_Application, nullptr, &id);
        if (e != vr::VRNotificationError_OK) LOG("notification failed: %d\n", e);
    };

    auto onClick = [&](const std::string &id) {
        LOG("click %s\n", id.c_str());
        if (id == "stop") {
            v.pending = true;
            v.pendingDevice.clear();
            pendingSince = Clock::now();
            spawnDetached({g_script, "stop"});
        } else if (id == "level") {
            v.level = !v.level;
            writeFile(g_stabFile, v.level ? "1\n" : "0\n");
        } else if (id == "refresh") {
            disc.start();
        } else if (id.rfind("dev:", 0) == 0) {
            std::string name = id.substr(4);
            bool active = v.cast.state == "casting" || v.cast.state == "starting";
            if (active && v.cast.device == name) return;
            v.pending = true;
            v.pendingDevice = name;
            pendingSince = Clock::now();
            v.lastDevice = name;
            writeFile(g_lastDeviceFile, name + "\n");
            spawnDetached({g_script, "start", name});
        }
        dirty = true;
    };

    auto stateTick = Clock::now();
    while (!g_stop) {
        vr::VREvent_t ev;
        while (vr::VRSystem()->PollNextEvent(&ev, sizeof ev))
            if (ev.eventType == vr::VREvent_Quit) { LOG("SteamVR quit\n"); g_stop = 1; }
        while (vr::VROverlay()->PollNextOverlayEvent(ov, &ev, sizeof ev)) {
            switch (ev.eventType) {
            case vr::VREvent_OverlayShown:
                visible = true;
                if (disc.age() > 15) disc.start();
                dirty = true;
                break;
            case vr::VREvent_OverlayHidden:
                visible = false;
                break;
            case vr::VREvent_MouseMove:
            case vr::VREvent_MouseButtonDown: {
                // Mouse origin is bottom-left; the drawing origin is top-left.
                double x = ev.data.mouse.x, y = H - ev.data.mouse.y;
                std::string hit;
                for (auto &b : buttons) if (b.r.contains(x, y)) hit = b.id;
                if (hit != v.hover) { v.hover = hit; dirty = true; }
                if (ev.eventType == vr::VREvent_MouseButtonDown && !hit.empty()) onClick(hit);
                break;
            }
            }
        }
        if (g_stop) break;

        auto now = Clock::now();
        if (now - stateTick > std::chrono::milliseconds(300)) {
            stateTick = now;
            CastState cs = readCastState();
            if (!(cs == v.cast)) {
                // Any new state from framecast.sh answers the pending click.
                v.cast = cs;
                v.pending = false;
                dirty = true;
            }
            if (v.pending && now - pendingSince > std::chrono::seconds(15)) { v.pending = false; dirty = true; }
            if (!(cs == notified)) {
                if (cs.state == "casting") notify("Casting to " + cs.device);
                else if (cs.state == "error") notify("Cast to TV: " + cs.message);
                else if (cs.state == "idle" && notified.state == "casting") notify("Stopped casting to " + notified.device);
                notified = cs;
            }
        }
        if (disc.takeChanged()) dirty = true;
        bool searching = disc.busy();
        if (searching != v.searching) { v.searching = searching; dirty = true; }

        if (dirty) {
            v.searched = disc.searched();
            v.devices = disc.devices();
            // Last used device first, then by name.
            std::stable_sort(v.devices.begin(), v.devices.end(), [&](const Device &a, const Device &b) {
                if ((a.name == v.lastDevice) != (b.name == v.lastDevice)) return a.name == v.lastDevice;
                return a.name < b.name;
            });
            buttons = drawPanel(v, panelSurf);
            panelTex.upload(panelSurf, ov);
            bool casting = v.cast.state == "casting";
            if (!thumbDrawn || casting != thumbCasting) {
                drawThumb(casting, thumbSurf);
                thumbTex.upload(thumbSurf, thumb);
                thumbCasting = casting;
                thumbDrawn = true;
            }
            dirty = false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(visible ? 30 : 100));
    }

    vr::VR_Shutdown();
    return 0;
}
