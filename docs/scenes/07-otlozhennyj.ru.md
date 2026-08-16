# Часть VII. Отложенный рендеринг

> **Цель главы.** Развязать геометрию и освещение так, чтобы стоимость кадра перестала быть
> произведением `геометрия × источники` и стала суммой. Построить G-буфер через множественные
> цели рендеринга, восстановить позицию из глубины без обращения матриц, и осветить сцену
> объёмами источников. И честно посчитать, где этот подход выигрывает, а где проигрывает.

---

## 7.1. Идея

Разделяем кадр на два этапа.

**Этап 1 — геометрический проход.** Растеризуем всю непрозрачную геометрию и записываем в
несколько целей то, что нужно знать о **видимой** поверхности в каждом пикселе: цвет, нормаль,
шероховатость, металличность, глубину. Освещение не считаем вовсе.

**Этап 2 — проход освещения.** Для каждого источника читаем эти свойства и добавляем его вклад.
Геометрию больше не трогаем.

```
Прямой:      стоимость ≈ (фрагменты × источники)
Отложенный:  стоимость ≈ фрагменты + (видимые пиксели × источники)
```

Разница принципиальная. Во-первых, произведение стало суммой. Во-вторых — и это часто важнее —
освещение теперь считается ровно один раз на **видимый** пиксель. Никакого overdraw: то, что
перекрыто, отсеялось тестом глубины ещё в первом проходе.

Цена: G-буфер надо записать и прочитать. Это память, и именно она определяет, окупится ли обмен.

---

## 7.2. Что класть в G-буфер

Правило одно: **минимум, достаточный для вычисления освещения**. Каждый лишний канал — это байты
на запись и байты на чтение, умноженные на число пикселей и на частоту кадров.

Наш выбор форматов ограничен: в `ImageFormat` есть `RGBA8_UNORM`, `RGBA16_FLOAT` и `R32_FLOAT`.
Ни `RG8`, ни `RGB10A2`, ни `RG16F` — то есть привычных упаковочных форматов нет. Работаем с тем,
что есть.

| Цель | Формат | Содержимое | Байт |
|---|---|---|---|
| 0 | `RGBA8_UNORM` | Альбедо (в sRGB-кодировке) · rgb, металличность · a | 4 |
| 1 | `RGBA16_FLOAT` | Нормаль в мировых координатах · xyz, шероховатость · w | 8 |
| 2 | `R32_FLOAT` | Линейная глубина в пространстве вида | 4 |
| — | `D32_FLOAT` | Вложение глубины: только для теста, никогда не читается | 4 |

**16 байт на пиксель** — компактно по меркам отложенного рендеринга.

Несколько решений здесь неочевидны и требуют объяснения.

**Альбедо хранится в sRGB-кодировке, а не в линейной.** Обычно наоборот. Но у нас цель
`RGBA8_UNORM` без аппаратного sRGB, и линейное альбедо в 8 битах даёт заметные ступеньки в тёмных
тонах — там, где sRGB-кодировка как раз и уплотняет градации. Поэтому: пишем то, что вернула
выборка, а декодируем в проходе освещения. Ноль дополнительной стоимости, качество лучше.

**Нормаль в `RGBA16_FLOAT`, а не упакованная в 8 бит.** Октаэдральная упаковка в две
восьмибитные компоненты — стандартный приём, но без формата `RG8` она заняла бы в `RGBA8` те же
два канала при худшем качестве: 8 бит на компоненту дают видимые ступени на гладких затенённых
поверхностях. Полукратные float по трём каналам расходуют 8 байт, зато нормаль точная, и заодно
остаётся канал под шероховатость.

**Линейная глубина отдельной целью, а не чтением буфера глубины.** Вот здесь — важное
ограничение нашей обёртки, и его надо назвать прямо.

> В обёртке изображение с назначением `DepthStencil` «отдыхает» в состоянии вложения глубины, а
> не в состоянии, пригодном для чтения шейдером. Привязать буфер глубины как текстуру через
> `setTexture()` — **нарушение**: на Vulkan это ошибка слоя валидации, на Metal сработает, и
> получится код, который «работает на Mac и падает на Windows».

Поэтому линейную глубину мы **пишем сами** в цель `R32_FLOAT`. Стоит 4 байта на пиксель, зато
работает на обоих бэкендах и даёт побочную выгоду: линейная глубина удобнее нелинейной для
восстановления позиции, для тумана и для SSAO в главе 11.

Что в G-буфер **не** кладём:

- **Позицию.** Три `float32` — это 12 байт ради того, что восстанавливается из глубины двумя
  умножениями. Классическая ошибка новичка.
- **Диффузную и зеркальную составляющие раздельно.** Выводятся из альбедо и металличности.
- **Тангенциальный базис.** Карта нормалей применяется в геометрическом проходе; дальше нужна
  только итоговая нормаль.

---

## 7.3. Создание целей

```cpp
constexpr ImageFormat kGBufferAlbedo = ImageFormat::RGBA8_UNORM;
constexpr ImageFormat kGBufferNormal = ImageFormat::RGBA16_FLOAT;
constexpr ImageFormat kGBufferDepth  = ImageFormat::R32_FLOAT;
constexpr ImageFormat kDepthFormat   = ImageFormat::D32_FLOAT;

struct GBuffer {
    std::shared_ptr<GImage> albedo;
    std::shared_ptr<GImage> normal;
    std::shared_ptr<GImage> linearDepth;
    std::shared_ptr<GImage> depthStencil;
};

GBuffer createGBuffer(const std::shared_ptr<Device>& device, uint32_t width, uint32_t height)
{
    auto make = [&](ImageFormat format, ImageUsage usage, const char* name) {
        ImageDesc desc{};
        desc.format = format;
        desc.width  = width;
        desc.height = height;
        desc.usage  = usage;
        desc.debugName = name;
        return device->createImage(desc);
    };

    GBuffer g;
    // ColorTarget — пишем в геометрическом проходе.
    // Sampled     — читаем в проходе освещения. Оба назначения нужны СРАЗУ:
    //               расширить их после создания нельзя ни на одном бэкенде.
    const ImageUsage rw = ImageUsage::ColorTarget | ImageUsage::Sampled;

    g.albedo       = make(kGBufferAlbedo, rw, "GBufferAlbedo");
    g.normal       = make(kGBufferNormal, rw, "GBufferNormal");
    g.linearDepth  = make(kGBufferDepth,  rw, "GBufferLinearDepth");
    g.depthStencil = make(kDepthFormat, ImageUsage::DepthStencil, "GBufferDepth");
    return g;
}
```

Пересоздавать при изменении размера окна — как и всё остальное, по размеру **свопчейна**.

---

## 7.4. Геометрический проход

Пайплайн объявляет три цели:

```cpp
PipelineDesc desc{};
desc.vertexFunction   = gbufferVertex;
desc.fragmentFunction = gbufferFragment;
desc.targetFormat = RenderTargetFormat::multiTarget(
    { kGBufferAlbedo, kGBufferNormal, kGBufferDepth },
    kDepthFormat);
desc.depthStencil = DepthStencilState::depthTestAndWrite();
desc.depthStencil.depthCompareOp = CompareOp::Greater;   // reverse-Z
desc.rasterizer.cullMode = CullMode::Back;
desc.debugName = "GBufferPipeline";
```

Число целей в `multiTarget` обязано совпадать с числом вложений в проходе **и** с числом выходов
фрагментного шейдера. Три места, снова.

Проход:

```cpp
std::shared_ptr<RenderPassDescriptor> gPass = helper::createRenderPassDescriptor();
gPass->setColorAttachment(0, gbuffer.albedo,      true, blackClear);
gPass->setColorAttachment(1, gbuffer.normal,      true, blackClear);
gPass->setColorAttachment(2, gbuffer.linearDepth, true, farClear);   // очистка дальностью
gPass->setDepthStencilAttachment(gbuffer.depthStencil, true, 0.0f, false, 0);
```

Индексы должны идти подряд с нуля, без пропусков.

**Очищайте цель линейной глубины значением дальней плоскости**, а не нулём. Пиксели, которых не
коснулась геометрия (небо), иначе окажутся на нулевом расстоянии от камеры, и любой источник
света их «осветит». Симптом — светящееся небо.

### Шейдер

```metal
#include <metal_stdlib>
using namespace metal;

struct MeshVertex {
    packed_float3 position;  float pad0;
    packed_float3 normal;    float pad1;
    packed_float2 uv;        packed_float2 pad2;
};

struct DrawConstants {
    float4x4 modelViewProjection;
    float4x4 modelView;       // нужен для линейной глубины и мировой нормали
    float4   material;        // x: шероховатость, y: металличность,
                              // z: есть альбедо, w: альфа-маска
};

struct GBufferVertexOut {
    float4 clipPosition [[position]];
    float3 worldNormal;
    float2 uv;
    float  viewDepth;          // положительное расстояние вдоль оси взгляда
};

vertex GBufferVertexOut gbuffer_vertex(const device MeshVertex* vertices  [[buffer(0)]],
                                       constant DrawConstants&  constants [[buffer(8)]],
                                       uint                     vid       [[vertex_id]])
{
    MeshVertex v = vertices[vid];
    const float4 local = float4(float3(v.position), 1.0);

    GBufferVertexOut out;
    out.clipPosition = constants.modelViewProjection * local;
    out.worldNormal  = float3(v.normal);
    out.uv           = float2(v.uv);

    // В правосторонней системе камера смотрит вдоль −Z, поэтому расстояние — это −z.
    out.viewDepth = -(constants.modelView * local).z;
    return out;
}

// Выход из нескольких целей описывается структурой с атрибутами [[color(n)]].
struct GBufferOut {
    float4 albedo      [[color(0)]];
    float4 normal      [[color(1)]];
    float  linearDepth [[color(2)]];
};

fragment GBufferOut gbuffer_fragment(GBufferVertexOut        in        [[stage_in]],
                                     constant DrawConstants& constants [[buffer(8)]],
                                     texture2d<float>        albedoMap [[texture(0)]],
                                     texture2d<float>        alphaMap  [[texture(1)]],
                                     sampler                 smp       [[sampler(0)]],
                                     bool                    facing    [[front_facing]])
{
    if (constants.material.w > 0.5) {
        if (alphaMap.sample(smp, in.uv).r < 0.5) discard_fragment();
    }

    float3 albedo = float3(1.0);
    if (constants.material.z > 0.5) {
        // Пишем БЕЗ декодирования: сохраняем sRGB-кодировку ради точности
        // в 8 битах. Декодируем в проходе освещения.
        albedo = albedoMap.sample(smp, in.uv).rgb;
    }

    float3 normal = normalize(in.worldNormal);
    if (!facing) normal = -normal;

    GBufferOut out;
    out.albedo      = float4(albedo, constants.material.y);   // a = металличность
    out.normal      = float4(normal, constants.material.x);   // w = шероховатость
    out.linearDepth = in.viewDepth;
    return out;
}
```

Структура выхода с `[[color(n)]]` — единственное синтаксическое отличие MRT от одиночной цели.
Номер должен совпадать с индексом вложения в проходе.

---

## 7.5. Восстановление позиции без обращения матриц

Проход освещения знает координату пикселя и линейную глубину. Нужна мировая позиция.

Обычный путь — умножить NDC-координату на обратную матрицу `viewProjection`. Но, как выяснилось в
главе 3, **в MSL нет `inverse()` для `float4x4`**, а считать обращение на CPU и передавать 64
байта каждый кадр — расточительно, когда есть путь проще.

Путь проще опирается на то, что линейная глубина у нас уже есть. Позиция в пространстве вида
восстанавливается из направления луча и расстояния, а направление луча — линейная функция
экранной координаты:

```metal
// tanHalfFov = tan(fovY * 0.5), передаётся в uniform-буфере
float3 viewPositionFromDepth(float2 uv, float linearDepth,
                             float tanHalfFov, float aspect)
{
    // UV → NDC. Y инвертируется: uv растёт вниз, NDC-Y — вверх.
    const float2 ndc = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);

    // Луч через центр пикселя, нормированный так, что z == −1.
    const float3 ray = float3(ndc.x * tanHalfFov * aspect,
                              ndc.y * tanHalfFov,
                              -1.0);

    // linearDepth — расстояние вдоль оси взгляда, ровно то, что записал
    // геометрический проход.
    return ray * linearDepth;
}
```

Мировая позиция получается умножением на **обратную матрицу вида**. Обращать её тоже не нужно:
обратная к матрице вида — это и есть матрица камеры «из вида в мир», и на CPU она строится
напрямую из положения и ориентации камеры:

```cpp
// lookAt даёт мир → вид. Обратная — вид → мир — собирается без обращения:
// поворотная часть транспонируется, перенос — это позиция камеры.
Mat4 inverseView(const Vec3& position, const Vec3& right,
                 const Vec3& up, const Vec3& forward)
{
    Mat4 m{};
    m[0] = right.x;   m[1] = right.y;   m[2]  = right.z;
    m[4] = up.x;      m[5] = up.y;      m[6]  = up.z;
    m[8] = -forward.x; m[9] = -forward.y; m[10] = -forward.z;
    m[12] = position.x; m[13] = position.y; m[14] = position.z;
    m[15] = 1.0f;
    return m;
}
```

Итого: ноль обращений матриц, две операции в шейдере, полная точность даже на San Miguel — потому
что линейная глубина в `R32_FLOAT` не теряет точность на дальних планах, в отличие от нелинейной.

---

## 7.6. Проход освещения: полный экран или объёмы

Два способа, и выбор между ними — это выбор между простотой и эффективностью.

### Вариант A: полноэкранный проход

Один полноэкранный треугольник, цикл по всем источникам внутри шейдера.

Плюс: один вызов отрисовки, никакого смешивания, все данные G-буфера читаются один раз.
Минус: каждый пиксель платит за все источники, включая те, что от него за километр. Возвращается
проблема из главы 6, только теперь без overdraw.

Годится до ~20 источников.

### Вариант B: объёмы источников

Для каждого источника рисуем сферу радиусом в его дальность действия, с аддитивным смешиванием.
Фрагментный шейдер выполняется **только для пикселей, накрытых сферой на экране**.

Это и есть настоящий отложенный рендеринг, и он работает в нашей обёртке без вычислительных
шейдеров.

```cpp
PipelineDesc lightDesc{};
lightDesc.vertexFunction   = lightVolumeVertex;
lightDesc.fragmentFunction = lightVolumeFragment;
lightDesc.targetFormat     = RenderTargetFormat::singleTarget(kHdrFormat);

// Вклады источников складываются.
lightDesc.blendStates = { BlendState::additive() };

// Тест глубины выключен, и рисуем ЗАДНИЕ грани сферы.
// Это делает проход корректным, даже когда камера внутри объёма источника —
// случай, на котором ломается наивная реализация с передними гранями.
lightDesc.depthStencil.depthTestEnabled  = false;
lightDesc.depthStencil.depthWriteEnabled = false;
lightDesc.rasterizer.cullMode = CullMode::Front;

lightDesc.debugName = "LightVolume";
```

Три настройки, каждая обязательна:

**`BlendState::additive()`** — вклады источников складываются, это физика: яркости суммируются.

**`CullMode::Front`** — рисуем изнанку сферы. Если рисовать лицевую сторону, то при заходе камеры
внутрь объёма передние грани окажутся за ближней плоскостью, будут отсечены, и источник погаснет.
Изнанка остаётся видимой всегда.

**Тест глубины выключен.** С включённым тестом изнанка сферы, находящаяся за геометрией, была бы
отброшена — вместе с освещением этой геометрии. Отбраковка ненужных пикселей делается в шейдере
по расстоянию, а не тестом глубины.

Более экономный вариант — трафаретный тест в два прохода: первым проходом пометить в трафарете
пиксели внутри объёма, вторым осветить только их. `StencilOpState` в обёртке есть, и формат
`D24_UNORM_S8_UINT` тоже. Но это удваивает число проходов на источник и требует общего буфера
глубины и трафарета — а мы используем `D32_FLOAT` ради reverse-Z. Компромисс не в нашу пользу;
оставляем как упражнение.

### Шейдер объёма

```metal
struct LightUniforms {
    float4x4 viewProjection;
    float4x4 inverseView;
    packed_float3 cameraPosition;  float tanHalfFov;
    float aspect;                  float _pad[3];
};

struct LightConstants {
    packed_float3 position;   float range;
    packed_float3 color;      float intensity;
    float4x4 volumeTransform;      // сфера единичного радиуса → мир
};

struct LightVolumeOut {
    float4 clipPosition [[position]];
};

vertex LightVolumeOut light_volume_vertex(
        const device MeshVertex*  sphere    [[buffer(0)]],
        constant LightUniforms&   scene     [[buffer(1)]],
        constant LightConstants&  light     [[buffer(8)]],
        uint                      vid       [[vertex_id]])
{
    MeshVertex v = sphere[vid];
    const float4 world = light.volumeTransform * float4(float3(v.position), 1.0);

    LightVolumeOut out;
    out.clipPosition = scene.viewProjection * world;
    return out;
}

fragment float4 light_volume_fragment(
        LightVolumeOut           in     [[stage_in]],
        constant LightUniforms&  scene  [[buffer(1)]],
        constant LightConstants& light  [[buffer(8)]],
        texture2d<float>         gAlbedo [[texture(0)]],
        texture2d<float>         gNormal [[texture(1)]],
        texture2d<float>         gDepth  [[texture(2)]],
        sampler                  smp     [[sampler(0)]])
{
    // [[position]] во фрагментном шейдере — координата В ПИКСЕЛЯХ,
    // начало в верхнем левом углу. Переводим в UV делением на размер.
    const float2 uv = in.clipPosition.xy / float2(gDepth.get_width(), gDepth.get_height());

    const float linearDepth = gDepth.sample(smp, uv).r;

    const float3 viewPosition = viewPositionFromDepth(uv, linearDepth,
                                                      scene.tanHalfFov, scene.aspect);
    const float3 worldPosition = (scene.inverseView * float4(viewPosition, 1.0)).xyz;

    const float3 toLight = float3(light.position) - worldPosition;
    const float  distanceSquared = dot(toLight, toLight);

    // Отбраковка вне дальности: пиксель накрыт сферой на экране,
    // но в пространстве до него далеко.
    if (distanceSquared > light.range * light.range) discard_fragment();

    const float4 albedoSample = gAlbedo.sample(smp, uv);
    const float4 normalSample = gNormal.sample(smp, uv);

    // Декодируем то, что сохраняли закодированным.
    const float3 albedo    = srgbToLinear(albedoSample.rgb);
    const float  metallic  = albedoSample.a;
    const float3 N         = normalize(normalSample.xyz);
    const float  roughness = max(normalSample.w, 0.03);

    const float3 V  = normalize(float3(scene.cameraPosition) - worldPosition);
    const float3 L  = toLight * rsqrt(max(distanceSquared, 1e-8));

    float attenuation = 1.0 / max(distanceSquared, 1e-4);
    const float ratio  = distanceSquared / (light.range * light.range);
    const float window = saturate(1.0 - ratio * ratio);
    attenuation *= window * window;

    const float3 radiance = float3(light.color) * light.intensity * attenuation;

    const float3 F0 = mix(float3(0.04), albedo, metallic);
    const float3 diffuseColor = albedo * (1.0 - metallic);

    return float4(shadeLight(N, V, L, radiance, diffuseColor, F0, roughness), 1.0);
}
```

Ключевая строка — перевод `[[position]]` в UV. Во фрагментном шейдере `[[position]]` содержит
**пиксельные** координаты с началом в верхнем левом углу, а не clip-пространство. Делением на
размер цели получаем UV, ориентированные так же, как текстурные координаты. Никакой инверсии Y
здесь не нужно — обе величины считаются сверху вниз.

`get_width()` / `get_height()` — методы текстуры в MSL; передавать размер отдельным параметром не
требуется.

### Отрисовка источников

```cpp
cmd->beginRenderPass(lightingPass);      // цель — HDR-буфер, clear = true
cmd->setRenderPipeline(lightVolumePipeline);
cmd->setVertexBuffer(0, sphereVertexBuffer);
cmd->setUniformBuffer(1, ShaderStage::Vertex,   lightUniforms);
cmd->setUniformBuffer(1, ShaderStage::Fragment, lightUniforms);
cmd->setTexture(0, ShaderStage::Fragment, gbuffer.albedo,      pointSampler);
cmd->setTexture(1, ShaderStage::Fragment, gbuffer.normal,      pointSampler);
cmd->setTexture(2, ShaderStage::Fragment, gbuffer.linearDepth, pointSampler);

for (const Light& light : visibleLights) {
    LightConstants constants = makeLightConstants(light);
    cmd->setPushConstants(ShaderStage::Vertex,   &constants, sizeof(constants));
    cmd->setPushConstants(ShaderStage::Fragment, &constants, sizeof(constants));
    cmd->drawIndexed(sphereIndexBuffer, IndexType::UInt16, sphereIndexCount);
}
cmd->endRenderPass();
```

**`LightConstants` — 96 байт** (две тройки с числами плюс матрица 4×4). Влезает в 128. Если
добавить что-то ещё, придётся перекладывать источники в uniform-буфер и передавать индекс.

**Сэмплер обязан быть `Nearest`.** G-буфер читается пиксель в пиксель, и линейная фильтрация
смешала бы соседние пиксели — то есть свойства **разных поверхностей**. На границах объектов это
даёт ореолы неправильного освещения. Один из тех багов, что выглядят «почти правильно» и потому
живут в проекте месяцами.

**Текстуры и uniform-буфер привязываются один раз до цикла.** Меняются только push-константы —
самая дешёвая из операций.

---

## 7.7. Считаем, окупилось ли

Разрешение 2560×1440, 60 кадров в секунду.

**Прямой рендеринг, 4 источника:**

```
запись цвета RGBA16F  3.7 млн × 8 Б  = 29.6 МБ
глубина (запись+чтение)              = 29.6 МБ
                                       ───────
                                       59 МБ/кадр → 3.5 ГБ/с
```

**Отложенный, любое число источников:**

```
запись G-буфера   3.7 млн × 16 Б = 59 МБ
глубина                          = 29.6 МБ
чтение G-буфера в проходе света  = 59 МБ
запись HDR                       = 29.6 МБ
                                   ───────
                                   177 МБ/кадр → 10.6 ГБ/с
```

Втрое больше трафика — это **фиксированная плата** за вход, независимо от числа источников.

Где точка окупаемости? Прямой рендеринг платит арифметикой: `фрагменты × источники × 70` операций.
Отложенный — трафиком плюс `видимые пиксели × источники × 70`. При overdraw около 2 (типичный
интерьер) отложенный подход начинает выигрывать примерно **с 5–8 источников**, а с объёмами
источников — ещё раньше, потому что там платят не все пиксели.

**На интегрированной графике картина другая.** При пропускной способности 100 ГБ/с эти 10.6 ГБ/с
— уже 10 % шины, которую GPU делит с процессором. Точка окупаемости сдвигается вправо, и при
десятке источников прямой рендеринг может остаться быстрее. Считайте под своё железо; в этом весь
смысл упражнения из главы 1.

### И отдельно — про Apple Silicon

Здесь надо быть особенно честным, потому что курс записывается на Mac.

Как разбиралось в главе 1, Apple Silicon — тайловая архитектура. Её сильная сторона в том, что
промежуточные результаты остаются в тайловой памяти и не попадают в VRAM. Отложенный рендеринг
на таких GPU может быть **дешевле**, чем на дискретной карте: G-буфер объявляется
«безпамятным» (`memoryless`), геометрический проход и проход освещения объединяются в
подпроходы одного прохода рендеринга, и G-буфер вообще никогда не покидает чип. Трафик
G-буфера — ноль.

**Наша обёртка этого не умеет.** В ней нет ни подпроходов, ни memoryless-хранилища, ни чтения из
цели рендеринга во фрагментном шейдере. Значит, отложенный рендеринг из этой главы работает на
Mac по «дискретной» схеме, с полной выгрузкой 16 байт на пиксель в память и полным чтением
обратно.

Это работает и это корректно. Но знайте: на M-серии вы платите за отложенный рендеринг заметно
больше, чем должны были бы. В главе 13 указано, что нужно добавить в обёртку, чтобы это исправить
— и это самая крупная по отдаче доработка из всего списка для платформы Apple.

---

## 7.8. Чего мы лишились

Отложенный рендеринг — это компромисс, и честный разбор обязан включать потери.

**MSAA становится недоступен.** Многовыборочное изображение нельзя объявить `Sampled` — это
запрещено и обёрткой, и обоими API. Значит, G-буфер не может быть многовыборочным. Сглаживание
переезжает в постобработку (FXAA, TAA — глава 11), и качество там принципиально ниже.

**Прозрачность не поддерживается вовсе.** В G-буфере на пиксель помещается ровно одна поверхность.
Прозрачные объекты приходится рисовать отдельным прямым проходом после освещения — то есть иметь
**два** набора шейдеров для одних и тех же материалов. Глава 9.

**Один тип материала на весь экран.** Кожа с подповерхностным рассеянием, ткань, волосы требуют
другой BRDF. В прямом проходе это просто другой пайплайн; в отложенном — либо дополнительный
канал-идентификатор с ветвлением в проходе освещения (дивергенция!), либо отдельные проходы.

**Расход памяти.** 16 байт на пиксель — при 2560×1440 это 59 МБ только под G-буфер, плюс HDR,
плюс глубина. Около 100 МБ, которые в прямой схеме не нужны.

Из-за этого набора многие современные движки используют **Forward+**: тайловое разбиение экрана
со списками источников, дающее сумму вместо произведения, но сохраняющее MSAA, прозрачность и
свободу материалов. Forward+ требует вычислительного шейдера, которого у нас нет.

---

## 7.9. Отладочная визуализация

Обязательный инструмент, а не украшение. Ошибка в G-буфере без него ищется вслепую.

```metal
fragment float4 gbuffer_debug(FullscreenOut in [[stage_in]],
                              constant uint& mode [[buffer(8)]],
                              texture2d<float> gAlbedo [[texture(0)]],
                              texture2d<float> gNormal [[texture(1)]],
                              texture2d<float> gDepth  [[texture(2)]],
                              sampler smp [[sampler(0)]])
{
    switch (mode) {
        case 1: return float4(gAlbedo.sample(smp, in.uv).rgb, 1.0);
        case 2: return float4(gAlbedo.sample(smp, in.uv).aaa, 1.0);   // металличность
        case 3: return float4(gNormal.sample(smp, in.uv).xyz * 0.5 + 0.5, 1.0);
        case 4: return float4(gNormal.sample(smp, in.uv).www, 1.0);   // шероховатость
        case 5: {
            const float d = gDepth.sample(smp, in.uv).r;
            return float4(float3(fract(d * 0.1)), 1.0);   // полосы каждые 10 единиц
        }
        default: return float4(0.0);
    }
}
```

Переключатель в ImGui. Что смотреть:

- **Нормали.** Должны быть плавными на гладких поверхностях. Резкая мозаика — не нормализованы
  или испорчены интерполяцией. Постоянный цвет — не записались.
- **Глубина полосами.** Полосы должны идти ровно и сгущаться вдаль. Отсутствие полос вдали —
  потеря точности; хаос — неверная матрица `modelView`.
- **Альбедо.** Должно выглядеть как сцена без освещения, ровно с той яркостью, что в файлах
  текстур.

---

## 7.10. Что обычно ломается

| Симптом | Причина |
|---|---|
| Небо ярко освещено | Цель линейной глубины очищена нулём, а не дальностью |
| Ореолы неправильного цвета на границах объектов | Линейный сэмплер вместо `Nearest` при чтении G-буфера |
| Источники освещают всё одинаково | Позиция восстанавливается неверно; проверьте отладкой глубины |
| Источник гаснет при заходе камеры внутрь | Рисуются передние грани сферы; нужен `CullMode::Front` |
| Освещение только на переднем плане | Включён тест глубины в проходе объёмов |
| Освещение не суммируется, видно только последний | Не задан `BlendState::additive()` |
| Ошибка при привязке буфера глубины как текстуры | Так делать нельзя — пишите линейную глубину сами |
| Ошибка про число целей | `multiTarget`, вложения прохода и выходы шейдера разошлись |
| Ступени на затенении гладких поверхностей | Нормаль упакована в 8 бит |
| Тёмные ступени в тенях | Альбедо записано линейным в `RGBA8` вместо sRGB-кодировки |
| Отложенный медленнее прямого | Мало источников. Посчитайте трафик — это ожидаемо |

---

## 7.11. Проверка

1. **Отладочный вывод каждой цели.** Все три выглядят осмысленно.
2. **Один источник, отложенный и прямой рендеринг.** Картинки должны совпасть с точностью до
   точности форматов. Расхождение — ошибка в восстановлении позиции или в декодировании.
3. **Проверка позиции.** Временно выведите `fract(worldPosition)` цветом. Должна получиться
   регулярная трёхмерная решётка, привязанная к геометрии и **не плывущая** при движении камеры.
   Плывёт — восстановление неверно.
4. **График времени кадра от числа источников.** У прямого рендеринга — линейный рост. У
   отложенного — почти горизонтальная линия. Точка пересечения и есть ответ на вопрос «когда
   переходить», полученный на вашем железе.
5. **Проверка трафика.** Отключите запись цели линейной глубины и измерьте разницу во времени
   кадра. Она покажет, упираетесь вы в пропускную способность или в арифметику.

---

## Итог

1. Отложенный рендеринг превращает `геометрия × источники` в сумму. Плата — фиксированный трафик
   G-буфера, около 16 байт на пиксель в нашей раскладке.
2. **Буфер глубины сэмплировать нельзя** — пишем линейную глубину в отдельную цель `R32_FLOAT`.
   Это ограничение обёртки, и обход даёт побочную пользу.
3. Позиция восстанавливается из линейной глубины и направления луча — **без единого обращения
   матрицы**, которого в MSL всё равно нет.
4. Объёмы источников: аддитивное смешивание, `CullMode::Front`, тест глубины **выключен**.
5. G-буфер читается **только** сэмплером `Nearest`.
6. Потери: нет MSAA, нет прозрачности, один тип материала, +100 МБ памяти.
7. **На Apple Silicon мы платим за отложенный рендеринг больше, чем должны**, потому что в
   обёртке нет подпроходов и memoryless-целей.

Дальше — [Часть VIII. Тени](08-teni.ru.md): без них сцена остаётся плоской, а объекты — парящими.
