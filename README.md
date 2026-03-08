# Obsidian Engine

> **Born of fire, shaped by reflection.**
> A pure Vulkan engine with an embedded Scheme runtime and a live, self-editing world.

---

### 🌋 Overview

**Obsidian** is a game engine written in **C**, forged atop **Vulkan**, and enlivened by an embedded **Scheme** interpreter.
It unites the precision of low-level graphics with the expressive power of Lisp — a living system where games can be *examined, rewritten, and extended* while running.

When magma erupts from a volcano and cools in an instant, it becomes **obsidian** — dark, smooth, and reflective.
In the same way, **Vulkan** provides the molten foundation, while **Scheme** cools and shapes it into clarity and form.
The result is an environment where **creation and introspection are one continuous act**.

---

###  Core Features

- **Vulkan renderer in pure C** — explicit, minimal, and fast
- **Embedded Scheme REPL** — modify, extend, and inspect your game live
- **In-game editor** — your game *is* the editor; create and tweak without leaving runtime
- **Hot reload** — assets, logic, shaders, and pipelines
- **Philosophy:** the engine that forges itself

If **Vulkan** is the forge, **Scheme** is the sculptor’s hand.


### Quickstart

```C
#include <obsidian/obsidian.h>

int sw = 1920;
int sh = 1080;

int main() {
    initWindow(sw, sh, "Program name");

    Font *jetb = load_font("JetBrainsMonoNerdFont-Regular.ttf", 40);

    keychord_bind(&keymap, "M-=", nextTheme,     "Next theme",     PRESS);
    keychord_bind(&keymap, "M--", previousTheme, "Previous theme", PRESS);

    loadThemeByName("modus-vivendi");

    while (!windowShouldClose()) {
        beginFrame();
        clear_background(CT.bg);
        fps(jetb, context.swapChainExtent.width - 300, 100, CT.error);
        endFrame();
    }

    return 0;
}
```

---

### 🚧 Under Construction

**Obsidian** is still cooling.
The first shards of reflection are forming — a renderer, a REPL, a spark of self-awareness.
Contributions, discussions, and ideas are welcome while the core takes shape.
