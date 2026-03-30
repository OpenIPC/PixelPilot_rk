#pragma once

void create_air_camera_video_menu(lv_obj_t * parent);
void create_air_camera_image_menu(lv_obj_t * parent);
void create_air_camera_recording_menu(lv_obj_t * parent);
void create_air_camera_isp_menu(lv_obj_t * parent);
void create_air_camera_fpv_menu(lv_obj_t * parent);
void create_air_camera_menu(lv_obj_t * parent,
                             lv_obj_t * video_page,
                             lv_obj_t * image_page,
                             lv_obj_t * recording_page,
                             lv_obj_t * isp_page,
                             lv_obj_t * fpv_page);
