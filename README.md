# video2ascii — 视频转彩色字符串画面

用 C 语言写的 Linux 命令行小工具：读取视频文件，**实时**把画面转换成
ANSI 真彩色字符串播放，可选同时用 SDL2 输出音频，左上角实时显示帧率。

## 依赖

- FFmpeg 开发库（`libavformat` / `libavcodec` / `libavutil` / `libswscale` / `libswresample`），建议 **FFmpeg >= 5.0**
- SDL2 开发库
- `pkg-config`

安装（任选其一）：

```bash
# Debian / Ubuntu
sudo apt install build-essential pkg-config \
    libavformat-dev libavcodec-dev libavutil-dev libswscale-dev libswresample-dev \
    libsdl2-dev

# Fedora
sudo dnf install gcc make pkgconf-pkg-config \
    ffmpeg-devel SDL2-devel

# Arch
sudo pacman -S base-devel pkgconf ffmpeg sdl2
```

## 编译

```bash
make
```

如果不想用 Makefile，也可以直接：

```bash
gcc -O2 -Wall -Wextra -std=c11 -o video2ascii video2ascii.c \
    $(pkg-config --cflags --libs libavformat libavcodec libavutil \
                               libswscale libswresample sdl2) -lm
```

## 运行

```bash
./video2ascii
```

流程：

1. 程序提示输入视频文件路径。
2. 检测该文件能否打开、是否含**可解码的视频流**：
   - 不是视频文件 → 打印错误并**回到上一步**让你重新输入。
   - 是视频文件 → 打印分辨率/时长/有无音频，进入下一步。
3. 若视频含音频，会问 `是否同时输出音频？（y/N）`：
   - `y` → 视频 + 音频（SDL2）
   - 回车/`n` → 仅视频
4. 开始播放，左上角显示实时帧率。
5. 播放中按 `q` 或 `Esc` 退出，`Ctrl+C` 也可。

建议在足够大的终端窗口里运行（至少 96 列 × 27 行）。

## 关于 768×432 输出

需求里的“输出画面 768×432”按下面的方式实现：

- 视频像素**先缩放**到 **768×432**（`OUT_W` × `OUT_H`）。
- 终端字符比像素“高”，所以把画面切成 **8×16 像素**一个字符单元：
  - 列数 = 768 / 8 = **96**
  - 行数 = 432 / 16 = **27**
- 即最终是 96×27 的彩色字符网格，正好对应 768×432 的画面，比例不变形。

想改变字符密度，改 `video2ascii.c` 顶部的宏即可：

| `CELL_W` / `CELL_H` | 字符网格 | 说明 |
|---|---|---|
| `8` / `16` | 96 × 27 | 默认，普通终端最合适 |
| `4` / `8`  | 192 × 54 | 更清晰，需要更大终端 |
| `8` / `8`  | 96 × 54 | 字符偏“瘦高” |
| `1` / `2`  | 768 × 216 | 极密，需超大终端 |

## 参数

| 宏 | 默认 | 含义 |
|---|---|---|
| `OUT_W` / `OUT_H` | `768` / `432` | 缩放后的像素分辨率 |
| `CELL_W` / `CELL_H` | `8` / `16` | 每个字符覆盖的像素块 |
| `TARGET_FPS` | `30` | 输出帧率 |
| `RAMP` | `" .:-=+*#%@"` | 亮度→字符映射表（暗→亮） |

## 实现要点

- **解码 / 缩放**：`libavformat` + `libavcodec` 解码，`libswscale` 缩放到 768×432 的 `RGB24`。
- **30 FPS 定时**：以帧时间戳（PTS）选帧并配合单调时钟节流：
  - 视频原帧率高于 30 → 自动丢帧；
  - 低于 30 → 自动重复上一帧；
  - 渲染跟不上时按墙钟跳过，保证音画同步。
- **彩色字符串**：每个字符单元取像素块平均 RGB，按亮度从 `RAMP`
  选字符，用 `\033[38;2;R;G;Bm` 输出 24 位真彩色。整帧一次性 `fwrite`，减少闪烁。
- **帧率显示**：每 0.5s 统计一次实际输出帧数，写到第 0 行左侧，显示为亮黄色 `FPS xx.x`。
- **音频**：`libavcodec` 解码音频，`libswresample` 转成 SDL 需要的 `S16`/所需声道与采样率，
  通过 `SDL_QueueAudio` 推入声卡；与视频共用同一条时间线，从而保持同步。

## 注意

- Linux 专用（用到了 `termios`、`unistd.h` 等 POSIX 接口），不兼容 Windows/macOS。
- 需要终端支持 24 位真彩色（绝大多数现代终端都支持）。
