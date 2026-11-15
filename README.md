<!--
# SPDX-FileCopyrightText: Copyright 2025 Eden Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later

# SPDX-FileCopyrightText: 2018 yuzu Emulator Project
# SPDX-License-Identifier: GPL-2.0-or-later
-->

# Lichii for The Duskbloods (Network Test)

<p align="center"><b>Choose your language / اختر اللغة</b></p>

<a id="english"></a>
<details name="lang">
<summary><b>English: open the guide</b></summary>

**This is a fork of [zortos293/Lichii](https://github.com/zortos293/Lichii) (nx2, build 177), tuned to run *The Duskbloods Network Test* (program `0400D1D02399A000`) on Windows PCs with an NVIDIA GPU and an Intel CPU.**
It pairs with the [playDB-CNT](https://github.com/sikoGD/playDB-CNT) launcher by sikoGD, which provides the local network-test servers the game needs to get in-game. The launcher (1.3.2) is included in the zip.

> Not affiliated with Nintendo, FromSoftware or the upstream projects. No games, keys or firmware are included. Use only files you have legally dumped yourself.

## What's new in v3

- **Rebased on zortos293/Lichii build 177** (v2 was build 117).
- **Sharper image with AMD FSR 3.1:** the game's own DLSS pass is replaced by FSR 3.1, fed with the game's real depth, motion vectors and jitter. The image is sharp and stable instead of blocky or blurry.
- **Optional NVIDIA DLSS** for RTX cards (real DLSS through NVIDIA Streamline), with Auto, DLAA, Quality, Balanced, Performance and Ultra Performance modes. Switch live in game between FSR 3.1 and the DLSS modes. FSR 3.1 stays the automatic fallback. See [NVIDIA DLSS](#nvidia-dlss-optional).
- **Fixes:**
  - the freeze / error 2153-0002 when the game saves
  - distorted, crackling sound
  - Bot Match crashes (the game's heap ran out)
  - red "laser" lines and brightness flicker in the hub
  - jagged, pixelated HUD and text when the internal resolution is above 1x (the window downscale now averages pixels)
- **Launcher playDB-CNT 1.3.2** included; offline mode works.

## Performance

Measured by the maintainer in actual play:

| | |
|---|---|
| CPU | Intel Core i5-12400F |
| GPU | NVIDIA GeForce RTX 3060 (12 GB), driver 616.56 |
| RAM | 16 GB |
| Match median, v3 with FSR 3.1 | **~46.7 fps** (40.7 fps on the previous build) |
| Average | **~50 fps** |
| Peak | **~60 fps** |
| Heaviest area | **36+ fps** (13-16 fps before this fork's optimizations) |

NVIDIA DLSS is not benchmarked yet. The emulator is mostly CPU-bound in this game, so a faster CPU helps more than a faster GPU.

## What this fork changes

| Area | Fix |
|---|---|
| Online check | Works with the playDB-CNT launcher: host redirect, loopback-only networking, the launcher's CA in the game's trust store, a BCAT file-name check (error 2122-0001) |
| Rendering | TAA black stripes (native stack spills), the fog volume turning white/NaN (per-layer 3D clears), white waves at the bottom of the screen (NN-upscaler arena clear), two missing Ampere shader instructions |
| Performance | Faster storage-buffer range lookups, clean buffer-sync skipping, and a sleeping (not spinning) GPU front thread |
| v2: upstream build 117 | The game's neural-network (HMMA) shaders run on the GPU's matrix units, NaN-safe float math, and 100+ other upstream fixes |
| v3: upstream build 177 | FSR 3.1 in place of the game's DLSS pass, optional NVIDIA DLSS with live switching, fixes for the save freeze (2153-0002), crackling sound, Bot Match crashes, hub laser lines and flicker, and pixelated HUD above 1x |

**Tested on:** an **NVIDIA RTX 3060** GPU with an **Intel Core i5-12400F** CPU. Other NVIDIA GPUs should behave the same; AMD and Intel GPUs are untested. Use a current GPU driver.

## Download

**[Download v3 (Lichii-Duskbloods.zip)](https://github.com/Mr-Tb/lichii-dusk/releases/tag/duskbloods-v3)**

Older builds: [all releases](https://github.com/Mr-Tb/lichii-dusk/releases).

## Installation

You need:
- Windows 10/11 x64 and a Vulkan 1.3 GPU (NVIDIA recommended, see above)
- Your own `prod.keys` and `title.keys`
- Your own Switch 2 firmware dump (tested with 23.0.1)
- Your own dump of *The Duskbloods Network Test*
- The **[playDB-CNT launcher](https://github.com/sikoGD/playDB-CNT)** (`Play.Duskbloods.exe`, included in the zip). Follow its README for its own setup.

Steps:

1. **Extract** `Lichii-Duskbloods.zip` to a folder with a short path, e.g. `C:\Games\Lichii-Duskbloods\`. Keep the `user` folder next to `lichii.exe`: it makes the emulator portable and already holds the tuned settings.
2. **Keys:** copy `prod.keys` and `title.keys` into `user\keys\`. Or start `lichii.exe` and use **Tools > Install Decryption Keys**.
3. **Firmware:** start `lichii.exe`, then **Tools > Install Firmware > From Folder** (or **From ZIP**) and pick your firmware dump. Close the emulator afterwards.
4. **Launcher:** set up [playDB-CNT](https://github.com/sikoGD/playDB-CNT) as its README describes. Then edit its `launcher_settings.json` so `emulator` points to this `lichii.exe`:
   ```json
   {
     "emulator": "C:\\Games\\Lichii-Duskbloods\\lichii.exe",
     "game": "C:\\Games\\THE DUSKBLOODS NETWORK TEST VER"
   }
   ```
   Use double backslashes. `game` is your game dump folder.
5. **Play:** run `Play.Duskbloods.exe` and press **Play**. Always start the game through the launcher: started directly, the online check fails.

The first minutes stutter while shaders compile; this improves with each session.

## NVIDIA DLSS (optional)

For NVIDIA RTX cards only. Without it, the emulator uses AMD FSR 3.1, which works on every GPU.

1. **Get NVIDIA's files** (not included in the zip): open **Emulation > Configure > Graphics**, tick **NVIDIA DLSS** and press **Download from NVIDIA** in its panel. Or copy `sl.interposer.dll`, `sl.common.dll`, `sl.dlss.dll` and `nvngx_dlss.dll` from the NVIDIA Streamline SDK 2.14.16 into `user\streamline\`.
2. **Turn it on:** tick **NVIDIA DLSS** and restart the emulator once. The first time, DLSS starts with the next launch.
3. **Switch live:** in game, click the upscaler button in the status bar (or press **F7**) to cycle between FSR3 and the DLSS modes: Auto, DLAA, Quality, Balanced, Performance, Ultra Performance. Right-click the button to pick one.

If DLSS cannot start, the emulator falls back to FSR 3.1.

## Recommended settings (already set in the zip)

| Setting | Value | Why |
|---|---|---|
| Graphics > Resolution | **1x** | The tested setting |
| Graphics > Anti-aliasing | **None** | FSR 3.1 / DLSS already handle it |
| Graphics > Advanced > GPU Mode | **Fast** | Big CPU saving |
| Extras > Asynchronous shader compilation | **On** | Fewer shader stutters |
| Graphics > VSync | **Mailbox** | Presenting does not block rendering |
| Async presentation | **Off** | Faster in this game |
| Window adapting filter | Bilinear | Cheapest |
| NVIDIA DLSS | Off | Turn it on for an RTX card, see above |
| Error applet | HLE | The LLE applet crashes the emulator |
| Docked mode | On | |

## Known issues

- Flickering on low-FPS devices at some higher resolutions.
- The first launch stutters while shaders rebuild for the new version.
- AMD and Intel GPUs are untested.
- DLSS needs an RTX card and a one-time download of NVIDIA's files.

## Troubleshooting

- **Stuck on "checking save data" or network errors:** start through `Play.Duskbloods.exe`, not `lichii.exe` directly.
- **Error 2153-0513 at boot:** a save got corrupted, e.g. by force-closing during a save. Move the game's save folder under `user\nand\user\save\` aside and start again.
- **Black screen after loading:** close and relaunch. A rare hang has been seen once.
- **Black screen on first boot after updating:** delete the shader cache in `user\cache\shader\`.
- **Flickering or broken geometry:** delete the line `EDEN_NX2_FRAMES_AHEAD=1` in `nx2_switches.txt` and restart.
- **No DLSS option or button:** it needs an RTX GPU and the four NVIDIA files in `user\streamline\`.
- **Missing DLL errors:** extract the whole zip. Do not run `lichii.exe` from inside the archive.

## Building from source

Branch [`dusk-v3r`](https://github.com/Mr-Tb/lichii-dusk/tree/dusk-v3r) (v2: [`dusk-v2`](https://github.com/Mr-Tb/lichii-dusk/tree/dusk-v2), v1: [`dusk-frizti`](https://github.com/Mr-Tb/lichii-dusk/tree/dusk-frizti)). Build with MSYS2 UCRT64 + clang, exactly as upstream Lichii.

## Credits and license

Built on [zortos293/Lichii](https://github.com/zortos293/Lichii), [Frizti/Lichii](https://github.com/Frizti/Lichii), [Eglantine](https://github.com/Djihads80/Eglantine), Eden, yuzu and Sudachi. The launcher and network-test servers are [playDB-CNT](https://github.com/sikoGD/playDB-CNT) by sikoGD. Licensed under GPL-3.0-or-later (see [LICENSE.txt](LICENSE.txt)). NVIDIA DLSS runs through NVIDIA Streamline (MIT license); NVIDIA's files are downloaded by the user, not shipped.

</details>

<a id="arabic"></a>
<details name="lang">
<summary><b>العربية: افتح الدليل</b></summary>

<div dir="rtl">

**هذا المشروع نسخة معدّلة (fork) من [zortos293/Lichii](https://github.com/zortos293/Lichii) (nx2، الإصدار 177)، ومحسّنة لتشغيل *The Duskbloods Network Test* (رقم البرنامج `0400D1D02399A000`) على أجهزة Windows بكرت شاشة NVIDIA ومعالج Intel.**
يعمل مع المشغّل [playDB-CNT](https://github.com/sikoGD/playDB-CNT) من sikoGD، والذي يوفّر خوادم الشبكة المحلية التي تحتاجها اللعبة للدخول. المشغّل (الإصدار 1.3.2) موجود داخل الملف المضغوط.

> المشروع غير تابع لـ Nintendo أو FromSoftware أو المشاريع الأصلية. لا يتضمن أي ألعاب أو مفاتيح أو firmware. استخدم فقط ملفات استخرجتها بنفسك بطريقة قانونية.

## الجديد في الإصدار الثالث

- **مبني على الإصدار 177 من zortos293/Lichii** (الإصدار الثاني كان مبنيًا على 117).
- **صورة أوضح مع AMD FSR 3.1:** استبدلنا مرحلة DLSS الخاصة باللعبة بـ FSR 3.1، ويستقبل العمق ومتجهات الحركة والـ jitter الحقيقية من اللعبة. النتيجة صورة حادّة وثابتة بدل الصورة المربّعة أو الضبابية.
- **NVIDIA DLSS اختياري** لكروت RTX (DLSS حقيقي عبر NVIDIA Streamline)، بأوضاع Auto وDLAA وQuality وBalanced وPerformance وUltra Performance. يمكنك التبديل بين FSR 3.1 وأوضاع DLSS أثناء اللعب مباشرة، ويبقى FSR 3.1 هو البديل التلقائي. راجع قسم [NVIDIA DLSS](#nvidia-dlss-اختياري).
- **إصلاحات:**
  - التجمّد / الخطأ 2153-0002 عند حفظ اللعبة
  - الصوت المشوّه والمتقطّع (طقطقة)
  - انهيار اللعبة في Bot Match (نفاد ذاكرة اللعبة)
  - الخطوط الحمراء ("الليزر") ووميض الإضاءة في الـ hub
  - تشوّه وتكسّر الواجهة والنصوص عند رفع الدقة الداخلية فوق 1x (تصغير الصورة للنافذة يأخذ الآن متوسط البكسلات)
- **مشغّل playDB-CNT الإصدار 1.3.2** مرفق، ووضع اللعب دون اتصال (offline) يعمل.

## الأداء

قياسات صاحب المشروع أثناء اللعب الفعلي:

| | |
|---|---|
| المعالج | Intel Core i5-12400F |
| كرت الشاشة | NVIDIA GeForce RTX 3060 (12 GB)، تعريف 616.56 |
| الذاكرة | 16 GB |
| الوسيط داخل المباريات، الإصدار الثالث مع FSR 3.1 | **حوالي 46.7 إطار/ثانية** (40.7 في الإصدار السابق) |
| المتوسط | **حوالي 50 إطار/ثانية** |
| الأعلى | **حوالي 60 إطار/ثانية** |
| أثقل منطقة | **أكثر من 36 إطار/ثانية** (كانت 13-16 قبل تحسينات هذه النسخة) |

لم نقِس أداء NVIDIA DLSS بعد. المحاكي في هذه اللعبة يعتمد على المعالج أكثر من كرت الشاشة، لذلك يفيد المعالج الأسرع أكثر من كرت شاشة أقوى.

## ما الذي تغيّر في هذه النسخة

| المجال | الإصلاح |
|---|---|
| فحص الاتصال | يعمل مع مشغّل playDB-CNT: إعادة توجيه الخوادم، شبكة محلية فقط، إضافة شهادة المشغّل إلى شهادات اللعبة الموثوقة، وإصلاح خطأ BCAT ‏(2122-0001) |
| الرسوميات | الخطوط السوداء (TAA)، الضباب الذي يتحوّل للأبيض، الموجات البيضاء أسفل الشاشة، وتعليمتان مفقودتان في shaders ‏Ampere |
| الأداء | بحث أسرع في نطاقات الذاكرة، تجاوز مزامنة البيانات غير المتغيّرة، وخيط GPU أمامي ينام بدل الانشغال بالانتظار |
| الإصدار الثاني: تحديث 117 | shaders الشبكة العصبية (HMMA) في اللعبة تعمل على وحدات المصفوفات في كرت الشاشة، حسابات أدق مع قيم NaN، وأكثر من 100 إصلاح آخر من المشروع الأصلي |
| الإصدار الثالث: تحديث 177 | FSR 3.1 بدل مرحلة DLSS الخاصة باللعبة، و NVIDIA DLSS اختياري مع التبديل المباشر، وإصلاح تجمّد الحفظ (2153-0002) وطقطقة الصوت وانهيار Bot Match وخطوط الليزر والوميض في الـ hub وتكسّر الواجهة فوق 1x |

**تمّت التجربة على:** كرت شاشة **NVIDIA RTX 3060** مع معالج **Intel Core i5-12400F**. يُفترض أن تعمل كروت NVIDIA الأخرى بنفس الطريقة، أما كروت AMD وIntel فلم تُجرَّب. استخدم أحدث تعريف لكرت الشاشة.

## التحميل

**[حمّل الإصدار الثالث (Lichii-Duskbloods.zip)](https://github.com/Mr-Tb/lichii-dusk/releases/tag/duskbloods-v3)**

الإصدارات الأقدم: [كل الإصدارات](https://github.com/Mr-Tb/lichii-dusk/releases).

## التثبيت

تحتاج إلى:
- Windows 10/11 بنظام 64-bit وكرت شاشة يدعم Vulkan 1.3 (يُنصح بـ NVIDIA)
- ملفّي `prod.keys` و`title.keys` الخاصين بك
- نسخة firmware خاصة بك من Switch 2 (تمّت التجربة على 23.0.1)
- نسختك من *The Duskbloods Network Test*
- **[مشغّل playDB-CNT](https://github.com/sikoGD/playDB-CNT)** ‏(`Play.Duskbloods.exe`، موجود داخل الملف المضغوط)، مع اتباع الشرح في صفحته لإعداده

الخطوات:

1. **فكّ الضغط** عن `Lichii-Duskbloods.zip` في مجلد بمسار قصير، مثل `C:\Games\Lichii-Duskbloods\`. أبقِ مجلد `user` بجانب `lichii.exe`: فهو يجعل المحاكي محمولًا ويحتوي على الإعدادات المحسّنة.
2. **المفاتيح:** انسخ `prod.keys` و`title.keys` إلى `user\keys\`، أو شغّل `lichii.exe` واختر **Tools > Install Decryption Keys**.
3. **الـ Firmware:** شغّل `lichii.exe`، ثم اختر **Tools > Install Firmware > From Folder** (أو **From ZIP**) وحدّد ملفات الـ firmware. ثم أغلق المحاكي.
4. **المشغّل:** جهّز [playDB-CNT](https://github.com/sikoGD/playDB-CNT) كما في شرحه، ثم عدّل ملف `launcher_settings.json` بحيث يشير `emulator` إلى `lichii.exe`:

<div dir="ltr">

```json
{
  "emulator": "C:\\Games\\Lichii-Duskbloods\\lichii.exe",
  "game": "C:\\Games\\THE DUSKBLOODS NETWORK TEST VER"
}
```

</div>

   استخدم شرطتين مائلتين `\\`. القيمة `game` هي مجلد اللعبة.

5. **اللعب:** شغّل `Play.Duskbloods.exe` واضغط **Play**. شغّل اللعبة دائمًا من المشغّل، لأن تشغيلها مباشرة يُفشل فحص الاتصال.

في الدقائق الأولى قد يحدث تقطيع بسيط أثناء تجهيز الـ shaders، ويتحسّن ذلك مع كل جلسة.

## NVIDIA DLSS (اختياري)

لكروت NVIDIA RTX فقط. بدونه يستخدم المحاكي AMD FSR 3.1، وهو يعمل على كل كروت الشاشة.

1. **حمّل ملفات NVIDIA** (غير موجودة في الملف المضغوط): افتح **Emulation > Configure > Graphics**، فعّل **NVIDIA DLSS** واضغط **Download from NVIDIA** في اللوحة الخاصة به. أو انسخ بنفسك الملفات `sl.interposer.dll` و`sl.common.dll` و`sl.dlss.dll` و`nvngx_dlss.dll` من NVIDIA Streamline SDK 2.14.16 إلى المجلد `user\streamline\`.
2. **فعّله:** ضع علامة على **NVIDIA DLSS** وأعد تشغيل المحاكي مرة واحدة. في المرة الأولى يبدأ DLSS مع التشغيل التالي.
3. **بدّل أثناء اللعب:** اضغط زر الـ upscaler في شريط الحالة أسفل النافذة (أو **F7**) للتنقّل بين FSR3 وأوضاع DLSS: Auto وDLAA وQuality وBalanced وPerformance وUltra Performance. اضغط بالزر الأيمن لاختيار وضع معيّن.

إذا لم يستطع DLSS العمل، يعود المحاكي تلقائيًا إلى FSR 3.1.

## الإعدادات المقترحة (مضبوطة مسبقًا في الملف المضغوط)

| الإعداد | القيمة | السبب |
|---|---|---|
| Graphics > Resolution | **1x** | الإعداد الذي تمّت تجربته |
| Graphics > Anti-aliasing | **None** | FSR 3.1 أو DLSS يتكفّلان بذلك |
| Graphics > Advanced > GPU Mode | **Fast** | توفير كبير في استهلاك المعالج |
| Extras > Asynchronous shader compilation | **On** | تقطيع أقل |
| Graphics > VSync | **Mailbox** | العرض لا يعطّل الرسم |
| Async presentation | **Off** | أسرع في هذه اللعبة |
| Window adapting filter | Bilinear | الأخفّ |
| NVIDIA DLSS | Off | فعّله إذا كان لديك كرت RTX (راجع الأعلى) |
| Error applet | HLE | نسخة LLE تُسقط المحاكي |
| Docked mode | On | |

## مشاكل معروفة

- وميض على الأجهزة ذات معدل الإطارات المنخفض عند بعض الدقّات العالية.
- أول تشغيل فيه تقطيع أثناء إعادة بناء الـ shaders للإصدار الجديد.
- كروت AMD وIntel لم تُجرَّب.
- DLSS يحتاج كرت RTX وتحميل ملفات NVIDIA مرة واحدة.

## حل المشاكل

- **التوقف عند "checking save data" أو أخطاء الشبكة:** شغّل اللعبة من `Play.Duskbloods.exe` وليس من `lichii.exe` مباشرة.
- **الخطأ 2153-0513 عند التشغيل:** ملف الحفظ تلف، مثلًا بسبب إغلاق اللعبة بالقوة أثناء الحفظ. انقل مجلد حفظ اللعبة داخل `user\nand\user\save\` إلى مكان آخر وأعد التشغيل.
- **شاشة سوداء بعد التحميل:** أغلق اللعبة وأعد تشغيلها (حدث تعليق نادر مرة واحدة).
- **شاشة سوداء أول تشغيل بعد التحديث:** احذف ذاكرة الـ shaders في `user\cache\shader\`.
- **وميض أو أشكال مشوّهة:** احذف السطر `EDEN_NX2_FRAMES_AHEAD=1` من الملف `nx2_switches.txt` ثم أعد تشغيل المحاكي.
- **لا يظهر خيار أو زر DLSS:** يحتاج كرت RTX والملفات الأربعة من NVIDIA داخل `user\streamline\`.
- **رسائل عن ملفات DLL مفقودة:** فكّ الضغط عن الملف كاملًا، ولا تشغّل `lichii.exe` من داخل الملف المضغوط.

## البناء من الكود المصدري

الفرع [`dusk-v3r`](https://github.com/Mr-Tb/lichii-dusk/tree/dusk-v3r) (الإصدار الثاني: [`dusk-v2`](https://github.com/Mr-Tb/lichii-dusk/tree/dusk-v2)، الإصدار الأول: [`dusk-frizti`](https://github.com/Mr-Tb/lichii-dusk/tree/dusk-frizti)). يُبنى باستخدام MSYS2 UCRT64 + clang مثل Lichii الأصلي تمامًا.

## الشكر والترخيص

مبني على [zortos293/Lichii](https://github.com/zortos293/Lichii) و[Frizti/Lichii](https://github.com/Frizti/Lichii) و[Eglantine](https://github.com/Djihads80/Eglantine) وEden وyuzu وSudachi. المشغّل وخوادم الشبكة من [playDB-CNT](https://github.com/sikoGD/playDB-CNT) لـ sikoGD. الترخيص: GPL-3.0-or-later (راجع [LICENSE.txt](LICENSE.txt)). يعمل NVIDIA DLSS عبر NVIDIA Streamline (ترخيص MIT)، وملفات NVIDIA يحمّلها المستخدم بنفسه ولا تُوزَّع مع المحاكي.

</div>

</details>
