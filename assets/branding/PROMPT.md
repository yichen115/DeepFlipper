# DeepFlipper Logo 生成说明

使用 imagegen 技能的 GPT Image 2 API 工作流生成，接口与凭据仅在调用时从用户本机配置读取，未写入项目。

参考：用户提供的鲸鱼娘形象及两种 Flipper 海豚风格。图案为社区项目视觉，不表示官方联名。最终 PNG 保留透明通道。

## 初版构图提示词

```text
Use case: logo-brand
Asset type: square mascot logo for the community hardware project DeepFlipper, used above a GitHub README title.
Primary request: A joyful affectionate group portrait of the DeepSeek whale girl hugging Flipper Zero's dolphin mascot, cheek to cheek, with two small pink hearts floating above them. Exactly two characters. Both characters should clearly be identifiable.
Input images: Image 1 is the whale girl's identity reference: keep her navy-to-cyan long wavy hair, whale-fin ear ornaments, white maid headband, blue-and-white maid dress, whale emblem on her apron, and curled blue whale tail. Image 2 is the Flipper dolphin identity reference: preserve the long dolphin snout, big friendly eye, little cybernetic patch/band on its forehead, rounded fins and playful shape; translate the monochrome pixel art into a smooth white/light-gray cartoon dolphin with small orange accents. Image 3 is an alternate official-style dolphin shape/color reference only; do not copy the pixelated/mosaic background.
Style/medium: polished cute chibi anime sticker illustration, crisp confident dark-navy outlines, simple flat colors with a little clean cel shading. Rounded friendly silhouettes, not realistic and not a pixel-art screenshot.
Composition/framing: centered compact icon-like embrace, full chibi whale girl with the dolphin nestled in her arms, faces touching, balanced visual weight and an immediately readable silhouette at thumbnail scale. Leave generous plain white margins, no frame, no background objects. Her whale emblem must remain visible. Clear separation of navy/cyan hair and the white/gray dolphin. Express warm smiling affection, tasteful and wholesome.
Color palette: deep ocean blue, cyan, white, a small Flipper-orange accent and pink hearts.
Text: none. Do not add slogans, letters, watermarks, signatures, or official partnership claims.
Constraints: one girl and one dolphin only; anatomically clean two arms and hands naturally holding the dolphin; no extra fins attached to the girl except her established whale features; do not turn the whale girl into an animal; no physical Flipper device or other props; no checkerboard, mosaic or busy scenery.
```

## 当前版本：蓝色海豚修订

在初版构图上通过 GPT Image 2 API 编辑，以用户提供的蓝色海豚图作为配色参考。
海豚主体改为亮天蓝／青蓝、腹部浅青，鲸鱼娘与贴贴构图保留。

```text
Use case: precise-object-edit
Image 1 is the existing DeepFlipper logo to edit. Image 2 is the dolphin COLOR reference only.
Change ONLY the dolphin's colors in Image 1 to match the blue Flipper dolphin reference: its head, upper snout, back, side, flippers and tail should be predominantly vivid sky-blue/cyan with medium-blue shading, while its lower snout and belly should be very pale cyan. Keep the gray cybernetic head patch as a small detail. Use tiny warm yellow accents where appropriate rather than prominent orange stripes. The dolphin must read as the cheerful BLUE mascot from Image 2, not a white robotic dolphin. Keep it visually separate from the girl's dark navy hair by using a lighter brighter blue.
Preserve everything else in Image 1: the exact whale girl's face, wink, navy/cyan hair, maid dress, apron whale emblem, tail, body proportions, hands and pose; the dolphin's smile, facial expression, silhouette and cheek-to-cheek hugging position; the two pink hearts; crisp dark-navy outlines and cute chibi sticker shading. Exactly one girl and one dolphin. Do not redraw the composition or add text or props. Preserve the transparent background and alpha channel, no solid background, no checkerboard. Do not reproduce the mosaic background from Image 2.
```
