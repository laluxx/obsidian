// window_x11.c
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <GLFW/glfw3.h>
#define GLFW_EXPOSE_NATIVE_X11
#include <GLFW/glfw3native.h>

void setWindowResizeIncrements_x11(GLFWwindow *window, int char_width, int char_height, 
                                    int min_width, int min_height) {
    if (!window) return;
    
    Display *display = glfwGetX11Display();
    Window x11_window = glfwGetX11Window(window);
    
    if (display && x11_window) {
        XSizeHints *hints = XAllocSizeHints();
        if (hints) {
            hints->flags = PResizeInc | PMinSize;
            
            // Set resize increments to character size
            hints->width_inc = char_width;
            hints->height_inc = char_height;
            
            // Set minimum size (in pixels)
            hints->min_width = min_width;
            hints->min_height = min_height;
            
            XSetWMNormalHints(display, x11_window, hints);
            XFree(hints);
        }
    }
}

/* void setWindowResizeIncrements_x11(GLFWwindow *window, int char_width, int char_height,  */
/*                                     int left_fringe, int right_fringe) { */
/*     if (!window) return; */
    
/*     Display *display = glfwGetX11Display(); */
/*     Window x11_window = glfwGetX11Window(window); */
    
/*     if (display && x11_window) { */
/*         XSizeHints *hints = XAllocSizeHints(); */
/*         if (hints) { */
/*             hints->flags = PResizeInc | PMinSize | PBaseSize; */
/*             hints->width_inc = char_width; */
/*             hints->height_inc = char_height; */
            
/*             // Base size is the fringes (non-resizable part) */
/*             hints->base_width = left_fringe + right_fringe; */
/*             hints->base_height = 0;  // Or your modeline/minibuffer height if fixed */
            
/*             // Minimum is 1 character + fringes */
/*             hints->min_width = char_width + left_fringe + right_fringe; */
/*             hints->min_height = char_height; */
            
/*             XSetWMNormalHints(display, x11_window, hints); */
/*             XFree(hints); */
/*         } */
/*     } */
/* } */
