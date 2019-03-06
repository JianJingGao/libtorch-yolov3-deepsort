#include <opencv2/opencv.hpp>

#include "imgui/imgui.h"
#include "imgui/imgui_impl_glfw.h"
#include "imgui/imgui_impl_opengl3.h"

#if defined(IMGUI_IMPL_OPENGL_LOADER_GL3W)

#include <GL/gl3w.h>    // Initialize with gl3wInit()

#elif defined(IMGUI_IMPL_OPENGL_LOADER_GLEW)
#include <GL/glew.h>    // Initialize with glewInit()
#elif defined(IMGUI_IMPL_OPENGL_LOADER_GLAD)
#include <glad/glad.h>  // Initialize with gladLoadGL()
#else
#include IMGUI_IMPL_OPENGL_LOADER_CUSTOM
#endif

#include <GLFW/glfw3.h>

#include "Detector.h"
#include "util.h"
#include "Tracker.h"
#include "Target.h"

using namespace std;
using namespace std::chrono;

static void glfw_error_callback(int error, const char *description) {
    cerr << "Glfw Error" << error << ": " << description << endl;
}

static GLFWwindow *setup_UI() {
    // Setup window
    glfwSetErrorCallback(glfw_error_callback);
    if (!glfwInit())
        return nullptr;

    // Decide GL+GLSL versions
#if __APPLE__
    // GL 3.2 + GLSL 150
    const char* glsl_version = "#version 150";
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);  // 3.2+ only
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);            // Required on Mac
#else
    // GL 3.0 + GLSL 130
    const char *glsl_version = "#version 130";
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
    //glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);  // 3.2+ only
    //glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);            // 3.0+ only
#endif

    // Create window with graphics context
    GLFWwindow *window = glfwCreateWindow(1280, 720, "Dear ImGui GLFW+OpenGL3 example", NULL, NULL);
    if (!window)
        return nullptr;
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1); // Enable vsync

    // Initialize OpenGL loader
#if defined(IMGUI_IMPL_OPENGL_LOADER_GL3W)
    bool err = gl3wInit() != 0;
#elif defined(IMGUI_IMPL_OPENGL_LOADER_GLEW)
    bool err = glewInit() != GLEW_OK;
#elif defined(IMGUI_IMPL_OPENGL_LOADER_GLAD)
    bool err = gladLoadGL() == 0;
#else
    bool err = false; // If you use IMGUI_IMPL_OPENGL_LOADER_CUSTOM, your loader is likely to requires some form of initialization.
#endif
    if (err) {
        fprintf(stderr, "Failed to initialize OpenGL loader!\n");
        return nullptr;
    }

    // Setup Dear ImGui context
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();

    // Setup Dear ImGui style
    ImGui::StyleColorsDark();
    //ImGui::StyleColorsClassic();

    // Setup Platform/Renderer bindings
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init(glsl_version);

    return window;
}

static std::array<int64_t, 2> orig_dim, inp_dim;

static void image_window(const char *name, GLuint texture,
                         bool *p_open = __null) {
    ImGui::SetNextWindowSizeConstraints({inp_dim[1], inp_dim[0]}, {orig_dim[1], orig_dim[0]});
    ImGui::Begin(name, p_open);
    ImGui::Image(reinterpret_cast<ImTextureID>(texture), ImGui::GetContentRegionAvail());
    ImGui::End();
}

int main(int argc, const char *argv[]) {
    if (argc != 2) {
        cerr << "usage: yolo-app <image path>" << endl;
        return -1;
    }

    cv::VideoCapture cap(argv[1]);
    if (!cap.isOpened()) {
        cerr << "Cannot open the video" << endl;
        return -2;
    }

    orig_dim[0] = static_cast<int64_t>(cap.get(cv::CAP_PROP_FRAME_HEIGHT));
    orig_dim[1] = static_cast<int64_t>(cap.get(cv::CAP_PROP_FRAME_WIDTH));
    for (size_t i = 0; i < 2; ++i) {
        auto factor = 1 << 5;
        inp_dim[i] = (orig_dim[i] / 3 / factor + 1) * factor;
    }
    Detector detector(inp_dim);
    Tracker tracker(orig_dim);

    auto window = setup_UI();
    if (!window) {
        cerr << "GUI failed" << endl;
        return -1;
    }

    std::map<int, Target> targets;

    GLuint texture[3];
    glGenTextures(sizeof(texture) / sizeof(texture), texture);

    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        static auto show_demo_window = false;
        static auto show_dets_window = false;
        static auto show_trks_window = false;
        static auto show_res_window = true;
        static auto show_target_window = true;
        static auto playing = false;

        ImGui::Begin("Control", nullptr, ImGuiWindowFlags_NoResize);
        ImGui::Text("Framerate: %.1f", ImGui::GetIO().Framerate);
        ImGui::Separator();
        ImGui::Checkbox("Show demo window", &show_demo_window);
        ImGui::Checkbox("Show detection window", &show_dets_window);
        ImGui::Checkbox("Show tracking window", &show_trks_window);
        ImGui::Checkbox("Show result window", &show_res_window);
        ImGui::Checkbox("Show target window", &show_target_window);
        ImGui::Separator();
        ImGui::Checkbox("Playing", &playing);
        auto next = ImGui::Button("Next");
        ImGui::End();

        if (show_demo_window)
            ImGui::ShowDemoWindow(&show_demo_window);

        static auto frame = 0;

        static vector<cv::Rect2f> dets;
        static vector<Track> trks;

        static cv::Mat image(orig_dim[0], orig_dim[1], CV_8UC3, {0, 0, 0});
        static cv::Mat dets_image = image.clone();
        static cv::Mat trks_image = image.clone();
        static cv::Mat ret_image = image.clone();

        if ((playing || next) && cap.read(image)) {
            dets = detector.detect(image);
            trks = tracker.update(dets);

            for (auto &[id, box]:trks) {
                if (targets.count(id)) {
                    targets[id].trajectories.emplace_back(frame, box);
                } else {
                    targets.emplace(id, Target(make_pair(frame, box), image(box).clone()));
                }
            }

            ++frame;
        }

        int64_t hovered = -1;
        if (show_target_window) {
            ImVec2 img_sz{50, 50};
            ImGui::Begin("Targets", &show_target_window);
            auto &style = ImGui::GetStyle();
            float window_visible_x2 = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x;
            for (auto it = targets.begin(); it != targets.end(); ++it) {
                auto &[id, t] = *it;
                ImGui::PushID(id);
                ImGui::Image(reinterpret_cast<ImTextureID>(t.snapshot_tex), {50, 50});
                if (ImGui::IsItemHovered()) {
                    ImGui::BeginTooltip();
                    ImGui::Text("ID: %d", id);
                    ImGui::Text("Start: %d", t.trajectories.front().first);
                    ImGui::Text("End: %d", t.trajectories.back().first);
                    ImGui::EndTooltip();
                    hovered = id;
                }
                ImGui::PopID();

                auto last_x2 = ImGui::GetItemRectMax().x;
                auto next_x2 = last_x2 + style.ItemSpacing.x
                               + img_sz.x; // Expected position if next button was on same line
                if (it != --targets.end() && next_x2 < window_visible_x2)
                    ImGui::SameLine();
            }
            ImGui::End();
        }

        if (show_dets_window) {
            image.copyTo(dets_image);
            for (auto &d:dets) {
                draw_bbox(dets_image, d);
            }
            mat_to_texture(dets_image, texture[0]);
            image_window("Detection", texture[0], &show_dets_window);
        }

        if (show_trks_window) {
            image.copyTo(trks_image);
            for (auto &t:trks) {
                draw_bbox(trks_image, t.box, to_string(t.id));
            }
            mat_to_texture(trks_image, texture[1]);
            image_window("Tracking", texture[1], &show_trks_window);
        }

        if (show_res_window) {
            image.copyTo(ret_image);
            for (auto &[id, t]:targets) {
                if (t.trajectories.back().first == frame - 1) {
                    cv::Scalar color;
                    if (id == hovered) {
                        color = {0, 0, 255};
                    } else {
                        color = {0, 0, 0};
                    }

                    draw_bbox(ret_image, t.trajectories.back().second, to_string(id), color);
                    draw_trajectories(ret_image, t.trajectories, color);
                }
            }
            mat_to_texture(ret_image, texture[2]);
            image_window("Result", texture[2], &show_res_window);
        }

        // Rendering
        ImGui::Render();
        int display_w, display_h;
        glfwMakeContextCurrent(window);
        glfwGetFramebufferSize(window, &display_w, &display_h);
        glViewport(0, 0, display_w, display_h);
        glClearColor(0.45f, 0.55f, 0.60f, 1.00f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        glfwMakeContextCurrent(window);
        glfwSwapBuffers(window);
    }

    // Cleanup
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();

    glfwDestroyWindow(window);
    glfwTerminate();

    return 0;
}