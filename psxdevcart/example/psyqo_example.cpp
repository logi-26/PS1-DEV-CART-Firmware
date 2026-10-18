/*
* PS1 DEV CART with PsyQo: example (build: see the Makefile in this folder)
*
* - psxdevcart: DevCartInit() runs the cartridge's kernel hook from a BIOS interrupt handler, so printf goes to the
*   computer, and live memory access, screenshots, hot reload, the profiler, the debugger and crash reports work;
* - psxpcdrv: a file of the computer read with PCopen/PCread (ps1transfer -e -f psyqo_example.ps-exe -c);
* - psxcomb: link play, a counter sent every frame, the other console's shown (the USB link: run it on both consoles,
*   then ps1transfer -n SERIAL1 -relay SERIAL2; or LINK_MODE=0: the serial link cable).
*
* This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
* If a copy of the MPL was not distributed with this file,
* You can obtain one at https://mozilla.org/MPL/2.0/.
*/

#include <stdint.h>

#include "psyqo/application.hh"
#include "psyqo/font.hh"
#include "psyqo/gpu.hh"
#include "psyqo/scene.hh"

extern "C" {
#include "common/syscalls/syscalls.h"
}
#include "psxcomb.h"
#include "psxdevcart.h"
#include "psxpcdrv.h"

#ifndef LINK_MODE
#define LINK_MODE COMB_LINK_USB
#endif

namespace {

class Example final : public psyqo::Application {
    void prepare() override;
    void createScene() override;

  public:
    psyqo::Font<> m_font;
    int m_devcart = 0;
    char m_file[40] = "(no file)";
    int m_rfd = -1, m_wfd = -1;
};

class ExampleScene final : public psyqo::Scene {
    void frame() override;
    uint32_t m_frame = 0, m_tx = 0, m_rx = 0, m_rxbuf = 0;
    bool m_reading = false;
};

Example example;
ExampleScene exampleScene;

}  // namespace

void Example::prepare() {
    psyqo::GPU::Configuration config;
    config.set(psyqo::GPU::Resolution::W320)
        .set(psyqo::GPU::VideoMode::AUTO)
        .set(psyqo::GPU::ColorMode::C15BITS)
        .set(psyqo::GPU::Interlace::PROGRESSIVE);
    gpu().initialize(config);
}

void Example::createScene() {
    m_font.uploadSystemFont(gpu());

    // After the application is prepared (PsyQo sets the BIOS interrupt handlers up then)
    m_devcart = DevCartInit();
    ramsyscall_printf("PsyQo example: PS1 DEV CART %s\n", m_devcart ? "found" : "not found");

    // A file of the computer (the folder served by ps1transfer -c)
    if (PCinit() == 0) {
        int fd = PCopen((char*)"hello.txt", PCDRV_READ, 0);
        if (fd >= 0) {
            int n = PCread(fd, m_file, sizeof(m_file) - 1);
            m_file[n > 0 ? n : 0] = 0;
            for (char* p = m_file; *p; p++)
                if (*p == '\r' || *p == '\n') *p = 0;
            PCclose(fd);
        }
    }

    // Link: the "sio:" device of psxcomb, asynchronous reads and writes
    AddCOMB();
    CombSetLink(LINK_MODE);
    m_rfd = syscall_open("sio:", O_RDONLY | O_NOWAIT);
    m_wfd = syscall_open("sio:", O_WRONLY | O_NOWAIT);
    pushScene(&exampleScene);
}

void ExampleScene::frame() {
    m_frame++;
    if (example.m_wfd >= 0 && !CombAsyncRequest(0)) {  // The previous write has ended
        m_tx = m_frame;
        syscall_write(example.m_wfd, &m_tx, 4);
    }
    if (example.m_rfd >= 0 && !CombAsyncRequest(1)) {  // The previous read has ended
        if (m_reading) m_rx = m_rxbuf;
        syscall_read(example.m_rfd, &m_rxbuf, 4);
        m_reading = true;
    }
    if (!(m_frame % 300)) ramsyscall_printf("frame %u, other console %u\n", m_frame, m_rx);

    psyqo::Color white = {{.r = 255, .g = 255, .b = 255}};
    example.gpu().clear({{.r = 16, .g = 40, .b = 72}});
    example.m_font.printf(example.gpu(), {{.x = 16, .y = 16}}, white, "PsyQo + PS1 DEV CART");
    example.m_font.printf(example.gpu(), {{.x = 16, .y = 40}}, white, "Cartridge: %s",
                          example.m_devcart ? "found" : "not found");
    example.m_font.printf(example.gpu(), {{.x = 16, .y = 56}}, white, "File: %s", example.m_file);
    example.m_font.printf(example.gpu(), {{.x = 16, .y = 72}}, white, "Frame %u, other console %u", m_frame, m_rx);
}

int main() { return example.run(); }
