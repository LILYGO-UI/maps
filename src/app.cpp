#include "app.hpp"

#include "app_identity.h"
#include "pages/map/map_view.hpp"

#include <memory>

namespace {

struct AppState {
    std::unique_ptr<MapViewModel> map_view_model;
    std::unique_ptr<MapView> map_view;
};

AppState app_state;

void app_open(const cm0_app_context_t *context)
{
    app_state.map_view_model = std::make_unique<MapViewModel>();
    app_state.map_view       = std::make_unique<MapView>(*app_state.map_view_model);
    app_state.map_view->create(context->root);
    app_state.map_view_model->start();
}

void app_close()
{
    if (app_state.map_view) app_state.map_view->destroy();
    app_state.map_view.reset();
    app_state.map_view_model.reset();
}

const cm0_app_descriptor_t app_descriptor{
    CM0_APP_API_VERSION, sizeof(cm0_app_descriptor_t), LILYGO_UI_TEMPLATE_APP_ID, app_open, app_close,
};

}  // namespace

extern "C" const cm0_app_descriptor_t *cm0_app_get_descriptor()
{
    return &app_descriptor;
}
