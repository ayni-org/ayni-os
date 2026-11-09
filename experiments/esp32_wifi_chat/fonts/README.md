# 字体来源与格式

使用 [GNU Unifont 16.0.04](https://unifoundry.com/pub/unifont/unifont-16.0.04/font-builds/unifont-16.0.04.hex.gz)，版权属于 Unifont 的各位贡献者。以 SIL Open Font License 1.1 使用，完整许可证见 [OFL-1.1.txt](OFL-1.1.txt)。派生构建产物名为 `ayni_font.bin`，不是重新发行同名字体；原 gzip 与生成文件留在被忽略的 `build/assets/`。

`prepare_font.py` 固定源文件 SHA-256，保留选定 Unicode 区间中的原始 8×16 / 16×16 字形。AYNF v2 头为 `<4sHHII>`（magic、版本、区间数、字形总数、每块字形数）；每个区间 `<III>` 为 start/end/first-glyph；随后每块 `<II>` 是绝对字节偏移/压缩长度，后接 zlib 块。每块最多 64 个 33 字节单元（1 字节宽度及 32 字节位图）。Canvas 从元数据查区间，不重复维护字符范围。
