# 第三方组件与许可证

Ayni 的原创代码和文档采用 [Apache License 2.0](LICENSE)。第三方组件和资源继续适用各自许可证，不因仓库许可证的选择而改变。

下表列出当前构建入口显式接入的主要依赖及资源。通过构建系统下载或由系统提供的依赖不作为源码副本提交到本仓库；分发包含它们的源码包、可执行程序或固件时，须附带实际使用版本要求的许可证和版权声明。

| 组件或资源 | 版本与接入位置 | 许可证与声明 |
|---|---|---|
| yyjson | 0.10.0；`cmake/AyniYyjson.cmake` | [MIT](https://github.com/ibireme/yyjson/blob/0.10.0/LICENSE) |
| MD4C | 0.5.2；`cmake/AyniMarkdown.cmake`，ESP 实验复用源码 | MIT；已保留[许可证副本](experiments/esp32_wifi_chat/third_party/md4c-LICENSE.md) |
| GoogleTest | 1.17.0；`cmake/AyniGoogleTest.cmake`，仅测试 | [BSD-3-Clause](https://github.com/google/googletest/blob/v1.17.0/LICENSE) |
| GNU Unifont | 16.0.04；ESP 实验构建字体资源 | SIL Open Font License 1.1；[字体说明](experiments/esp32_wifi_chat/fonts/README.md)与[许可证副本](experiments/esp32_wifi_chat/fonts/OFL-1.1.txt) |
| ESP-IDF | 6.0.3；ESP 构建工具链及 SDK | [Apache-2.0](https://github.com/espressif/esp-idf/blob/v6.0.3/LICENSE)；SDK 内第三方组件以各自声明为准 |
| esp_littlefs | 1.23.1；`hosts/esp_idf/idf_component.yml` 与 ESP 锁文件 | [MIT](https://github.com/joltwallet/esp_littlefs/blob/v1.23.1/LICENSE)；其携带的 littlefs 另按 BSD-3-Clause，见下载组件中的 `src/littlefs/LICENSE.md` |
| libcurl | Native 系统开发包；实际版本随构建环境记录 | [curl license](https://curl.se/docs/copyright.html)；TLS 等传递依赖以实际链接版本的声明为准 |
| SQLite | Native 系统开发包；实际版本随构建环境记录 | [SQLite public-domain dedication](https://www.sqlite.org/copyright.html) |

此表说明源码接入与资源来源，不替代某个二进制或固件发布包的完整传递依赖清单。字体、工具链及依赖的生成文件仍按现有忽略规则处理；本次公开复制不发布固件或设备数据。
