// Independent consumer entry. Its runtime selects the ESP Host and physical placement.
void run_portable_s3();
extern "C" void app_main() {
    run_portable_s3();
}
