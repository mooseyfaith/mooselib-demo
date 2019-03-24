
#define DEBUG_EDITOR

#include <default.h>
#include <mesh.h>
#include <tga.h>

u32 const Main_Window_ID = 0;

struct State : Default_State {
    Mesh pawn_mesh;
    Mesh sphere_mesh;
    Mesh cube_mesh;
    //Mesh inverse_cube_mesh;
    Mesh clip_background_quad_mesh;
    Frame_Buffer shadow_map_frame_buffer;
    
    struct {
        GLuint program_object;
        
        union {
            struct {
                struct {
                    GLint world_to_environment;
                    GLint map;
                } Environment;
                
                GLint Object_To_World;
            } uniform;
            
            GLint uniforms[sizeof(uniform) / sizeof(GLint)];
        };
        
        vec3f position;
        u32 level_of_detail_count;
    } environment_probe;
    
    struct {
        GLuint texture_object;
        Pixel_Dimensions resolution;
    } environment_map;
    
    struct {
        GLuint program_object;
        
        union {
            struct {
                GLint skybox_cube_map;
                GLint Object_To_World;
                GLint Clip_To_World;
            } uniform;
            
            GLint uniforms[sizeof(uniform) / sizeof(GLint)];
        };
    } skybox_shader;
    
    Frame_Buffer render_to_texture_frame_buffer;
    
    GLuint skybox_cube_map_object;
    
    struct {
        mat4x3 to_world;
    } player;
};

APP_INIT_DEC(application_init) {
    State *state;
    DEFAULT_STATE_INIT(State, state, platform_api);
    load_default_shader(state, platform_api, true, true);
    
    // load config:
    // - main window state
    // - camera state
    // - debug camera state
    {
        auto config = platform_api->read_entire_file(S("config.bin"), &state->transient_memory.allocator);
        if (config.count) {
            defer { free_array(&state->transient_memory.allocator, &config); };
            
            auto it = config;
            state->main_window_area = *next_item(&it, Pixel_Rectangle);
            state->camera.to_world = *next_item(&it, mat4x3f);
            state->debug.camera.to_world = *next_item(&it, mat4x3f);
        }
        else {
            state->main_window_area = { -1, -1, 1280, 720 };
        }
    }
    
    // load assets
    {
        auto source = platform_api->read_entire_file(S("meshs/chibi.glm"), &state->transient_memory.allocator);
        defer { free_array(&state->transient_memory.allocator, &source); };
        
        state->pawn_mesh = make_mesh(source, &state->persistent_memory.allocator);
    }
    
    {
        auto source = platform_api->read_entire_file(S("meshs/sphere.glm"), &state->transient_memory.allocator);
        defer { free_array(&state->transient_memory.allocator, &source); };
        
        state->sphere_mesh = make_mesh(source, &state->persistent_memory.allocator);
    }
    
    {
        auto source = platform_api->read_entire_file(S("meshs/cube.glm"), &state->transient_memory.allocator);
        defer { free_array(&state->transient_memory.allocator, &source); };
        
        state->cube_mesh = make_mesh(source, &state->persistent_memory.allocator);
    }
    
    {
        auto source = platform_api->read_entire_file(S("meshs/clip_background_quad.glm"), &state->transient_memory.allocator);
        defer { free_array(&state->transient_memory.allocator, &source); };
        
        state->clip_background_quad_mesh = make_mesh(source, &state->persistent_memory.allocator);
    }
    
    state->player.to_world = MAT4X3_IDENTITY;
    
    state->shadow_map_frame_buffer = make_frame_buffer({ 1024, 1024 });
    
    {
        glGenTextures(1, &state->environment_map.texture_object);
        
        glBindTexture(GL_TEXTURE_CUBE_MAP, state->environment_map.texture_object);
        
        state->environment_map.resolution = { 1024, 1024 };
        for (u32 i = 0; i < 6; i++)
        {
            glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + i, 
                         0, GL_RGB, state->environment_map.resolution.width, state->environment_map.resolution.height, 0, GL_RGB, GL_UNSIGNED_BYTE, 0);
        }
        
        glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);  
        
        glGenerateMipmap(GL_TEXTURE_CUBE_MAP);
        
        s32 level_base;
        glGetTexParameteriv(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_BASE_LEVEL, &level_base);
        
        s32 level_max;
        glGetTexParameteriv(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAX_LEVEL, &level_max);
        
        u32 max_size = state->environment_map.resolution.width;
        u32 p = level_base + (bit_count_of(max_size) - 1);
        u32 q = min(p, level_max);
        state->environment_probe.level_of_detail_count = q;
    }
    
    state->render_to_texture_frame_buffer = make_frame_buffer(state->environment_map.resolution, false);
    
    state->environment_probe.program_object = load_shader(state, platform_api, ARRAY_WITH_COUNT(state->environment_probe.uniforms), S(MOOSELIB_PATH "/shaders/debug_environment_map.shader.txt"),
                                                          EMPTY_STRING,
                                                          S("Environment.world_to_environment, Environment.map, Object_To_World"));
    state->environment_probe.position = vec3f{0, 10, 0};
    
    state->skybox_shader.program_object = load_shader(state, platform_api, ARRAY_WITH_COUNT(state->skybox_shader.uniforms), S(MOOSELIB_PATH "/shaders/skybox.shader.txt"),
                                                      EMPTY_STRING,
                                                      S("skybox_cube_map, Object_To_World, Clip_To_World"));
    
    {
        glGenTextures(1, &state->skybox_cube_map_object);
        glBindTexture(GL_TEXTURE_CUBE_MAP, state->skybox_cube_map_object);
        
        Texture skybox_side;
        bool ok = tga_load_texture(&skybox_side, S("Daylight Box_Pieces/Daylight Box_Right.tga"), platform_api->read_entire_file, &state->transient_memory.allocator, state->skybox_cube_map_object, GL_TEXTURE_CUBE_MAP, GL_TEXTURE_CUBE_MAP_POSITIVE_X);
        assert(ok);
        
        ok = tga_load_texture(&skybox_side, S("Daylight Box_Pieces/Daylight Box_Left.tga"), platform_api->read_entire_file, &state->transient_memory.allocator, state->skybox_cube_map_object, GL_TEXTURE_CUBE_MAP, GL_TEXTURE_CUBE_MAP_NEGATIVE_X);
        assert(ok);
        
        ok = tga_load_texture(&skybox_side, S("Daylight Box_Pieces/Daylight Box_Top.tga"), platform_api->read_entire_file, &state->transient_memory.allocator, state->skybox_cube_map_object, GL_TEXTURE_CUBE_MAP, GL_TEXTURE_CUBE_MAP_POSITIVE_Y);
        assert(ok);
        
        ok = tga_load_texture(&skybox_side, S("Daylight Box_Pieces/Daylight Box_Bottom.tga"), platform_api->read_entire_file, &state->transient_memory.allocator, state->skybox_cube_map_object, GL_TEXTURE_CUBE_MAP, GL_TEXTURE_CUBE_MAP_NEGATIVE_Y);
        assert(ok);
        
        ok = tga_load_texture(&skybox_side, S("Daylight Box_Pieces/Daylight Box_Front.tga"), platform_api->read_entire_file, &state->transient_memory.allocator, state->skybox_cube_map_object, GL_TEXTURE_CUBE_MAP, GL_TEXTURE_CUBE_MAP_POSITIVE_Z);
        assert(ok);
        
        ok = tga_load_texture(&skybox_side, S("Daylight Box_Pieces/Daylight Box_Back.tga"), platform_api->read_entire_file, &state->transient_memory.allocator, state->skybox_cube_map_object, GL_TEXTURE_CUBE_MAP, GL_TEXTURE_CUBE_MAP_NEGATIVE_Z);
        assert(ok);
        
        glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
        
        glGenerateMipmap(GL_TEXTURE_CUBE_MAP);
    }
    
    return state;
}

void draw_object_to_world_transform(mat4x3 object_to_world) {
    draw_line(object_to_world.translation, object_to_world.translation + object_to_world.columns[0], make_rgba32(1, 0, 0));
    draw_line(object_to_world.translation, object_to_world.translation + object_to_world.columns[1], make_rgba32(0, 1, 0));
    draw_line(object_to_world.translation, object_to_world.translation + object_to_world.columns[2], make_rgba32(0, 0, 1));
}

void render_sky(State *state, mat4x3f world_to_camera, mat4f camera_to_clip) {
    glUseProgram(state->skybox_shader.program_object);
    
    glUniform1i(state->skybox_shader.uniform.skybox_cube_map, 0);
    
    glActiveTexture(GL_TEXTURE0 + 0);
    glBindTexture(GL_TEXTURE_CUBE_MAP, state->skybox_cube_map_object);
    
    auto clip_to_world = make_inverse_unscaled_transform(world_to_camera) * make_inverse_perspective_projection(camera_to_clip);
    
    glUniformMatrix4fv(state->skybox_shader.uniform.Clip_To_World, 1, GL_FALSE, clip_to_world);
    
    draw(state->clip_background_quad_mesh.batch, 0);
}

void render_scene(State *state, bool override_program = false, GLint object_to_world_uniform = -1) {
    
    if (!override_program) {
        glUseProgram(state->default_shader.program_object);
        object_to_world_uniform = state->default_shader.uniform.Object_To_World;
        
        u32 texture_slot = 0;
        glUniform1i(state->default_shader.uniform.Material.diffuse_map, texture_slot);
        glActiveTexture(GL_TEXTURE0 + texture_slot++);
        glBindTexture(GL_TEXTURE_2D, state->blank_texture.object);
        
        glUniform1i(state->default_shader.uniform.Material.normal_map, texture_slot);
        glActiveTexture(GL_TEXTURE0 + texture_slot++);
        glBindTexture(GL_TEXTURE_2D, state->blank_normal_map.object);
        
        glUniform1i(state->default_shader.uniform.Shadow.map, texture_slot);
        glActiveTexture(GL_TEXTURE0 + texture_slot++);
        glBindTexture(GL_TEXTURE_2D, state->shadow_map_frame_buffer.depth_attachment_texture_object);
        
        glUniform1i(state->default_shader.uniform.Environment.map, texture_slot);
        glActiveTexture(GL_TEXTURE0 + texture_slot++);
        glBindTexture(GL_TEXTURE_CUBE_MAP, state->environment_map.texture_object);
        
        glUniform1i(state->default_shader.uniform.Environment.level_of_detail_count, state->environment_probe.level_of_detail_count);
    }
    
    // render pawn
    {
        if (!override_program) {
            glUniform1f(state->default_shader.uniform.Material.gloss, 0.3f);
            glUniform1f(state->default_shader.uniform.Material.metalness, 0.0f);
            glUniform4fv(state->default_shader.uniform.Material.specular_color, 1, vec4f{1, 1, 0, 1});
            glUniform4fv(state->default_shader.uniform.Material.diffuse_color, 1, vec4f{1, 0, 0, 1});
        }
        
        glUniformMatrix4x3fv(object_to_world_uniform, 1, GL_FALSE, state->player.to_world);
        draw(state->pawn_mesh.batch, 0);
    }
    
    auto gold = vec4f{1.00, 0.71, 0.29, 1.00 };
    
    // render ground
    {
        if (!override_program) {
            glUniform1f(state->default_shader.uniform.Material.gloss, 0.3f);
            glUniform1f(state->default_shader.uniform.Material.metalness, 0.0f);
            glUniform4fv(state->default_shader.uniform.Material.specular_color, 1, gold);
            glUniform4fv(state->default_shader.uniform.Material.diffuse_color, 1, vec4f{0.4, 0.4, 0.4, 1});
        }
        
        f32 thickness = 0.2f;
        
        glUniformMatrix4x3fv(object_to_world_uniform, 1, GL_FALSE, make_transform(QUAT_IDENTITY, vec3f{0, -thickness * 0.5f, 0}, { 100, thickness, 100 }));
        draw(state->cube_mesh.batch, 0);
    }
    
    // render cubes and spheres
    {
        u32 n = 16;
        for (u32 i = 0; i < n; i++) {
            if (!override_program) {
                glUniform1f(state->default_shader.uniform.Material.gloss, 0.3f);
                glUniform1f(state->default_shader.uniform.Material.metalness, 1.0f);
                glUniform4fv(state->default_shader.uniform.Material.specular_color, 1, gold);
                
                f32 diffuse = i / cast_v(f32, n);
                
                glUniform4fv(state->default_shader.uniform.Material.diffuse_color, 1, make_vec4_scale(diffuse));
            }
            
            auto t = make_transform(make_quat(VEC3_Y_AXIS, 2 * Pi32 * i / n), {});
            t.translation = transform_point(t, vec3f{ 10, 3, 0 });
            
            glUniformMatrix4x3fv(object_to_world_uniform, 1, GL_FALSE, t);
            
            if ((i % 2) == 0)
                draw(state->cube_mesh.batch, 0);
            else
                draw(state->sphere_mesh.batch, 0);
        }
        
    }
    
    // render environment probe for debugging
    {
        if (!override_program) {
            glUseProgram(state->environment_probe.program_object);
            
            glUniformMatrix4x3fv(state->environment_probe.uniform.Environment.world_to_environment, 1, GL_FALSE, make_inverse_unscaled_transform(make_transform(QUAT_IDENTITY, state->environment_probe.position)));
            
            glUniform1i(state->environment_probe.uniform.Environment.map, 0);
            glActiveTexture(GL_TEXTURE0 + 0);
            glBindTexture(GL_TEXTURE_CUBE_MAP, state->environment_map.texture_object);
            
            object_to_world_uniform = state->environment_probe.uniform.Object_To_World;
            
            glUniformMatrix4x3fv(object_to_world_uniform, 1, GL_FALSE, make_transform(QUAT_IDENTITY, state->environment_probe.position));
            
            draw(state->sphere_mesh.batch, 0);
        }
    }
}

APP_MAIN_LOOP_DEC(application_main_loop) {
    auto state = cast_p(State, app_data_ptr);
    auto ui = &state->ui;
    auto transient_allocator = &state->transient_memory.allocator;
    
    bool do_quit = !default_init_frame(state, input, platform_api, &delta_seconds);
    
    {
        Platform_Window window = platform_api->display_window(platform_api, Main_Window_ID, S("mooselib demo"), &state->main_window_area, true, state->main_window_is_fullscreen, 0.0f);
        
        do_quit |= window.was_destroyed;
        
        if (do_quit) {
            // save congif (see init)
            u8_array config = {};
            *grow_item(transient_allocator, &config, Pixel_Rectangle) = state->main_window_area;
            
            if (state->debug.is_active)
                *grow_item(transient_allocator, &config, mat4x3f) = state->debug.backup_camera_to_world;
            else
                *grow_item(transient_allocator, &config, mat4x3f) = state->camera.to_world;
            
            *grow_item(transient_allocator, &config, mat4x3f) = state->debug.camera.to_world;
            
            platform_api->write_entire_file(S("config.bin"), config);
            
            return Platform_Main_Loop_Quit;
        }
        default_debug_camera(state, input, window, delta_seconds);
        
        vec4f clear_color = vec4f{ 0.0, 0.5, 0.5, 1.0 };
        default_window_begin(state, window, clear_color);
        defer { default_window_end(state); };
        
        
        // update player position
        
        if (!state->debug.is_active || state->debug.use_game_controls) {
            vec3f direction = {};
            if (input->keys['W'].is_active)
                direction.z += -1.0f;
            
            if (input->keys['S'].is_active)
                direction.z += 1.0f;
            
            if (input->keys['A'].is_active)
                direction.x += -1.0f;
            
            if (input->keys['D'].is_active)
                direction.x += 1.0f;
            
            direction = normalize_or_zero(direction);
            
            if (squared_length(direction) > 0.0f)
            {
                mat4x3f camera_direction_to_world;
                camera_direction_to_world.forward = normalize(cross(state->camera.to_world.right, VEC3_Y_AXIS));
                camera_direction_to_world.right   = normalize(cross(VEC3_Y_AXIS, camera_direction_to_world.forward));
                camera_direction_to_world.up      = normalize(cross(state->camera.to_world.forward, state->camera.to_world.right));
                camera_direction_to_world.translation = {};
                
                direction = transform_direction(camera_direction_to_world, direction);
                
                f32 cos_alpha = dot(state->player.to_world.forward, direction);
                
                if (cos_alpha < 1.0) {
                    
                    f32 alpha = acos(cos_alpha);
                    
                    ui_write(ui, 5, ui->center_y, S("cos(alpha): %, alpha: %"), f(cos_alpha), f(alpha));
                    
                    draw_line(state->player.to_world.translation + VEC3_Y_AXIS, state->player.to_world.translation + direction * 3 + VEC3_Y_AXIS, make_rgba32(1));
                    
                    if (abs(alpha) <= delta_seconds * Pi32 * 2.0f) {
                        state->player.to_world.forward = direction;
                    }
                    else {
                        alpha = delta_seconds * Pi32 * 2.0f;
                        
                        if (dot(state->player.to_world.right, direction) < 0)
                            alpha *= -1;
                        
                        auto rot = make_transform(make_quat(VEC3_Y_AXIS, alpha));
                        state->player.to_world.forward = normalize(transform_direction(rot, state->player.to_world.forward));
                    }
                    state->player.to_world.right = cross(state->player.to_world.up, state->player.to_world.forward);
                    
                    draw_line(state->player.to_world.translation + VEC3_Y_AXIS, state->player.to_world.translation + state->player.to_world.right * 5 + VEC3_Y_AXIS, make_rgba32(1, 0, 0));
                    draw_line(state->player.to_world.translation + VEC3_Y_AXIS, state->player.to_world.translation + state->player.to_world.forward * 5 + VEC3_Y_AXIS, make_rgba32(0, 0, 1));
                }
            }
            
            state->player.to_world.translation += direction * delta_seconds * 20.0f;
        }
        
        // draw origin
        draw_object_to_world_transform(MAT4X3_IDENTITY);
        
        if (state->debug.is_active) {
            auto cursor = ui_text(ui, 0, ui->height - 12, S("DEBUG\n"));
            
            if (state->debug.use_game_controls)
                ui_text(ui, &cursor, S("Controling Game\n"));
            else
                ui_text(ui, &cursor, S("Controling Debug Camera\n"));
            
            ui_write(ui, &cursor, S("FPS: %\n"), f(1 / delta_seconds));
        }
        
        static f32 light_animation_time = 0;
        light_animation_time += delta_seconds;
        vec3f light_pos = { 2 * sin(light_animation_time), 25, 5 };
        f32 light_k = .005f;
        draw_circle(light_pos, 1.0f, state->camera.to_world.forward, make_rgba32(1));
        
        // upload lights
        {
            glBindBuffer(GL_UNIFORM_BUFFER, state->light_uniform_buffer_object);
            auto light_block = cast_p(Lighting_Uniform_Block, glMapBuffer(GL_UNIFORM_BUFFER, GL_WRITE_ONLY));
            
            light_block->global_ambient_color = make_vec4(0.2f, 0.2f, 0.2f) *0;
            
            f32 global_attenuation = 0.3f;
            light_block->parameters[0] = normalize_or_zero(make_vec4(1.0f, -1.0f, 0.0f));
            light_block->colors[0] = make_vec4(1.0f, 1.0f, 1.0f) * global_attenuation;
            light_block->directional_light_count = 1;
            
            draw_circle(vec3f{0, 20, 0}, .5f, state->camera.to_world.forward, make_rgba32(light_block->colors[0]));
            draw_line(vec3f{0, 20, 0}, vec3f{0, 20, 0} + make_vec3_cut(light_block->parameters[0]) * 5, make_rgba32(light_block->colors[0]));
            
            light_block->point_light_count = 1;
            light_block->parameters[1] = make_vec4(light_pos.x, light_pos.y, light_pos.z, light_k);
            light_block->colors[1] = make_vec4(1.0f, 1.0f, 1.0f, 1.0f);
            
            draw_circle(light_pos, 1.0f, state->camera.to_world.forward, make_rgba32(light_block->colors[0]));
            
            glUnmapBuffer(GL_UNIFORM_BUFFER);
            glBindBuffer(GL_UNIFORM_BUFFER, 0);
        }
        
        // update shadow map
        mat4f world_to_shadow;
        {
            glClearColor(1.0, 1.0, 1.0, 1.0);
            glDisable(GL_SCISSOR_TEST);
            bind_frame_buffer(state->shadow_map_frame_buffer);
            
            glUseProgram(state->shadow_map_shader.program_object);
            
            // shadow referes to clip space of the shadow map
            
            // light camera renders a squared canvas
            auto light_to_shadow = make_perspective_fov_projection(Pi32 * 0.5f, 1.0f);
            
            // light looks down
            auto light_to_world = make_transform(make_quat(VEC3_X_AXIS, Pi32 * -.5f), light_pos);
            auto world_to_light = make_inverse_unscaled_transform(light_to_world);
            
            world_to_shadow = light_to_shadow * world_to_light;
            
            glUniformMatrix4fv(state->shadow_map_shader.World_To_Shadow_Map, 1, GL_FALSE, world_to_shadow);
            
            draw_object_to_world_transform(light_to_world);
            
            render_scene(state, true, state->shadow_map_shader.Object_To_World);
            
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
        }
        
        // update environment_map
        {
            glUseProgram(state->default_shader.program_object);
            glUniformMatrix4fv(state->default_shader.uniform.Shadow.world_to_shadow, 1, GL_FALSE, world_to_shadow);
            
            glBindFramebuffer(GL_FRAMEBUFFER, state->render_to_texture_frame_buffer.object);
            glViewport(0, 0, state->environment_map.resolution.width, state->environment_map.resolution.height);
            glDisable(GL_SCISSOR_TEST);
            
            glClearColor(clear_color.r, clear_color.g, clear_color.b, clear_color.a);
            
            for (u32 i = 0; i < 6; i++)
            {
                glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_CUBE_MAP_POSITIVE_X + i, state->environment_map.texture_object, 0);
                
                mat4x3f world_to_probe;
                
                auto eye = state->environment_probe.position;
                
                switch (GL_TEXTURE_CUBE_MAP_POSITIVE_X + i) {
                    case GL_TEXTURE_CUBE_MAP_POSITIVE_X: {
                        world_to_probe = make_look_at({}, VEC3_X_AXIS, -VEC3_Y_AXIS);
                    } break;
                    
                    case GL_TEXTURE_CUBE_MAP_NEGATIVE_X: {
                        world_to_probe = make_look_at({}, -VEC3_X_AXIS, -VEC3_Y_AXIS);
                    } break;
                    
                    case GL_TEXTURE_CUBE_MAP_POSITIVE_Y: {
                        world_to_probe = make_look_at({}, VEC3_Y_AXIS, VEC3_Z_AXIS);
                    } break;
                    
                    case GL_TEXTURE_CUBE_MAP_NEGATIVE_Y: {
                        world_to_probe = make_look_at({}, -VEC3_Y_AXIS, -VEC3_Z_AXIS);
                    } break;
                    
                    case GL_TEXTURE_CUBE_MAP_POSITIVE_Z: {
                        world_to_probe = make_look_at({}, VEC3_Z_AXIS, -VEC3_Y_AXIS);
                    } break;
                    
                    case GL_TEXTURE_CUBE_MAP_NEGATIVE_Z: {
                        world_to_probe = make_look_at({}, -VEC3_Z_AXIS, -VEC3_Y_AXIS);
                    } break;
                }
                
                world_to_probe.translation = eye;
                world_to_probe = make_inverse_unscaled_transform(world_to_probe);
                
                auto probe_to_clip = make_perspective_fov_projection(Pi32 * .5f, 1.0f);
                
                auto clip_to_probe = make_inverse_perspective_projection(probe_to_clip);
                auto probe_to_world = make_inverse_unscaled_transform(world_to_probe);
                
                vec3f a = get_clip_to_world_point(probe_to_world, clip_to_probe, vec3f{-1, -1, 0});
                vec3f b = get_clip_to_world_point(probe_to_world, clip_to_probe, vec3f{ 1, -1, 0});
                vec3f c = get_clip_to_world_point(probe_to_world, clip_to_probe, vec3f{ 1,  1, 0});
                vec3f d = get_clip_to_world_point(probe_to_world, clip_to_probe, vec3f{-1,  1, 0});
                
                // draws one side of the purple cube
                // just to see if its a correct cube (so the environmap renders prperly in 360°)
                draw_line(a, b, make_rgba32(1, 0, 1));
                draw_line(b, c, make_rgba32(1, 0, 1));
                draw_line(c, d, make_rgba32(1, 0, 1));
                draw_line(d, a, make_rgba32(1, 0, 1));
                
                // override camera
                upload_camera_block(state->camera_uniform_buffer_object, world_to_probe, probe_to_clip, eye);
                
                glClear(GL_DEPTH_BUFFER_BIT);
                
                render_sky(state, world_to_probe, probe_to_clip);
                render_scene(state);
            }
            
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            
            glBindTexture(GL_TEXTURE_CUBE_MAP, state->environment_map.texture_object);
            glGenerateMipmap(GL_TEXTURE_CUBE_MAP);
        }
        
        // render final scene
        upload_camera_block(state);
        
        {
            set_auto_viewport(state->main_window_area.size, state->main_window_area.size, clear_color);
            
            glUseProgram(state->default_shader.program_object);
            
            glUniformMatrix4x3fv(state->default_shader.uniform.Environment.world_to_environment, 1, GL_FALSE, make_inverse_unscaled_transform(make_transform(QUAT_IDENTITY, state->environment_probe.position)));
            
            glUniformMatrix4fv(state->default_shader.uniform.Shadow.world_to_shadow, 1, GL_FALSE, world_to_shadow);
            
            
            render_sky(state, state->camera.world_to_camera, state->camera.to_clip_projection);
            render_scene(state);
        }
    }
    
    return Platform_Main_Loop_Continue;
}