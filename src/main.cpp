#include "ui/MainWindow.hpp"

#include <curl/curl.h>
#include <gst/gst.h>
#include <gtk/gtk.h>

#include <memory>

int main(int argc, char** argv) {
    gst_init(&argc, &argv);
    curl_global_init(CURL_GLOBAL_DEFAULT);

    GtkApplication* app = gtk_application_new("dev.uvc.chunkrecorder", G_APPLICATION_DEFAULT_FLAGS);
    std::unique_ptr<uvc::MainWindow> window;
    g_signal_connect(app, "activate", G_CALLBACK(+[](GtkApplication* app, gpointer data) {
                         auto* holder = static_cast<std::unique_ptr<uvc::MainWindow>*>(data);
                         if (!*holder) {
                             *holder = std::make_unique<uvc::MainWindow>(app);
                         }
                         (*holder)->present();
                     }),
                     &window);

    const int exit_code = g_application_run(G_APPLICATION(app), argc, argv);
    window.reset();
    g_object_unref(app);
    curl_global_cleanup();
    return exit_code;
}
