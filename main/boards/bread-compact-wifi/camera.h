// camera.h
#ifndef CAMERA_H
#define CAMERA_H

#include "esp_camera.h"
#include "esp_http_server.h"


class Camera {

public:
    Camera();
    esp_err_t init();
    esp_err_t jpg_httpd_handler(httpd_req_t *req);

private:
    camera_config_t camera_config;

    static size_t jpg_encode_stream(void * arg, size_t index, const void* data, size_t len);
};

#endif // CAMERA_H