#include "bindings.h"

NB_MODULE(waveshare_host, m) {
    m.doc() = "Waveshare host testing native bindings";

    init_sysdb(m);
    init_buffers(m);
}
