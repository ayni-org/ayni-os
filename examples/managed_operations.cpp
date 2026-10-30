// The identical application runs through either Host selected by the runtime.
#include "support.h"
int main(int argc, char **argv) {
    ayni::runtime::examples::HostConfig host;
    if (argc == 2) {
        host = {ayni::runtime::examples::HostConfig::Kind::native, argv[1]};
    } else if (argc != 1) {
        return 1;
    }
    ayni::runtime::examples::Fixture fixture;
    auto status = fixture.open({}, ayni::runtime::examples::Model::mock,
                               ayni::runtime::examples::Software::work, host);
    for (const auto id : {"software.sum", "software.events"}) {
        if (status == AYNI_OK) {
            status = fixture.grant(ayni::examples::text(id), ayni::examples::empty_scope());
        }
    }
    uint64_t id = 0;
    if (status == AYNI_OK) {
        status = fixture.create(ayni::examples::goal(fixture, "example.managed"), id);
    }
    if (status == AYNI_OK) {
        status = fixture.wait_task(id);
    }
    return ayni::examples::finish("managed_operations (sum and scalar stream)", status, fixture);
}
