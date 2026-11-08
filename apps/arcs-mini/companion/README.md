# 生生原生语音形象

`shengsheng_avatar_get(name)` 返回静态、只读的 `lisa_ui_anim_ext_config_t`。
应用启用 `CONFIG_SHENGSHENG_AVATAR` 后，可用这些动画替换原语音桌面的表情，
继续使用原桌面的对话字幕、唤醒及语音播放生命周期。

这部分提供形象与表情，不运行 Lua 养成逻辑，也不修改声音唤醒模型。
精灵沿用 `../miniapps/tamagotchi.lua` 的原创尖耳成年形象，按 4 倍像素绘制。

| 表情名称 | 动作 |
| --- | --- |
| `neutral`、`idle`、空指针及未知名称 | 呼吸、眨眼 |
| `wakeup`、`listening` | 聆听提示和轻微起伏 |
| `wait`、`thinking` | 三点思考动画 |
| `speaking`、`talking` | 三档张嘴动画 |
| `sleepy`、`sleep` | 闭眼与睡眠提示 |
| `happy`、`laugh`、`laughing`、`excited`、`love`、`loving`、`smile` | 弯眼微笑 |
| `angry`、`anger` | 生气表情 |
| `sad`、`cry`、`crying` | 难过和泪滴 |

动画只使用循环段，`enter` 和 `exit` 为空；帧描述符为连续 `lv_img_dsc_t` 数组，
可直接交给现有 `lisa_ui_anim`／`lisa_ui_anim_ext` 播放。静态配置没有共享可变状态。

## 重新生成

从仓库根目录运行，只需要 Python 3 标准库：

```sh
python3 apps/arcs-mini/companion/generate_avatar.py
python3 apps/arcs-mini/companion/generate_avatar.py --check
python3 apps/arcs-mini/companion/generate_avatar.py --preview tmp/shengsheng-preview.png
```

生成产物 `shengsheng_avatar_data.inc` 随源码保存，固件构建无需运行生成器、安装图像库或联网。
预览按从左到右、从上到下的顺序显示：待机、聆听、思考、说话、睡眠、开心、生气、难过。
说话预览为闭嘴帧，实际循环还有两档张嘴帧。

## 图像格式

每帧为 144×144 的 `LV_IMG_CF_INDEXED_2BIT`，透明、深墨、LCD 及外壳共四色。
格式与本仓库 LVGL8 `src/draw/lv_img_decoder.c` 一致：

- 16 字节 BGRA 调色板（`lv_color32_t`）；透明色 alpha 为 0，其他三色 alpha 为 255。
- 每行 36 字节，每字节从高位到低位排列四个 2 位像素。
- 每帧 5,200 字节，18 个去重帧共 93,600 字节像素，保存在 Flash。
- 每段动画具有独立的连续描述符数组，可复用同一只读像素帧；不复制整帧到堆，也不需要 PNG 解压缓冲。

生成器对每个像素进行打包／解包一致性检查，`--check` 同时检查已保存的产物。
