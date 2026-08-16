# Часть VI. Проходы и состояния

**Выпуски 15–17.** Строим многопроходную схему: рисуем в собственные цели, читаем их в
следующем проходе, включаем прозрачность, глубину и сглаживание. К концу части рендерер
перестаёт быть «однопроходным демо» и становится архитектурой, на которой строятся реальные
схемы освещения.

---

## Выпуск 15. Оффскрин-цели и несколько целей за проход

### 15.1. Две возможности, которые нужны вместе

**Оффскрин-цель** — изображение, в которое рисуем и которое потом читаем как текстуру. Без
неё невозможна пост-обработка: чтобы применить эффект к кадру, кадр надо где-то иметь.

**Несколько целей (MRT)** — фрагментный шейдер пишет в несколько изображений за один проход.
Так работает отложенное освещение: за один проход по геометрии записываются цвет, нормали и
параметры материала, а свет считается потом, по экрану.

Обе упираются в одно: до сих пор проход умел только одну цель, и она обязана была быть
изображением свопчейна.

### 15.2. Что мешало

В коде прохода стояло:

```cpp
if (index != 0) {
    throw std::runtime_error("поддерживается только цветовое вложение 0");
}
```

и

```cpp
if (!swapChain) {
    throw std::runtime_error("вложение не из свопчейна");
}
```

Оба ограничения снимаются вместе, потому что MRT почти всегда пишет в оффскрин-цели.

### 15.3. Ключ прохода растёт

```cpp
struct RenderPassKey {
    std::vector<RenderPassAttachmentKey> colors;   // было одно поле
    RenderPassAttachmentKey depth;
    VkSampleCountFlagBits samples;
};
```

Каждое вложение несёт **своё** состояние покоя. Практический смысл: проход может писать
одновременно в изображение свопчейна и в оффскрин-текстуру, и первое закончит в состоянии
«готово к показу», а второе — «готово к чтению шейдером».

### 15.4. Куда переехал кэш фреймбуферов

Раньше он жил на свопчейне. Теперь проход может состоять **только** из оффскрин-целей, о
которых свопчейн ничего не знает. Кэш переезжает на устройство:

```cpp
VkFramebuffer acquireFramebuffer(VkRenderPass renderPass,
                                 const std::vector<VkImageView>& attachments,
                                 uint32_t width, uint32_t height);
void invalidateFramebuffersUsing(VkImageView view);
```

Ответственность за инвалидацию — на том, кто уничтожает представление: свопчейн при
пересоздании, изображение в деструкторе.

Полезное общее правило: **кэш живёт там, где живёт то, что его инвалидирует.** Проходы зависят
от форматов — кэш на устройстве, переживает ресайз. Фреймбуферы зависят от представлений —
инвалидируются вместе с ними.

### 15.5. Синхронизация между проходами

Записали в текстуру в одном проходе, читаем в следующем. Кто гарантирует, что запись видна?

Не барьер в коде приложения. Две внешние зависимости прохода:

```cpp
// [вход] не писать вложения, пока их не отпустил показ и пока их не дочитал предыдущий проход
dependencies[0].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                               VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
dependencies[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
dependencies[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
dependencies[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                                VK_ACCESS_COLOR_ATTACHMENT_READ_BIT;

// [выход] записи цвета должны быть видны фрагментному шейдеру следующего прохода
dependencies[1].srcSubpass = 0;
dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
dependencies[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
dependencies[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
dependencies[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
dependencies[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
```

Вместе с состоянием покоя это и есть механизм, благодаря которому приложение не пишет барьеров.
Второй проход просто сэмплит текстуру — и это работает.

### 15.6. Текстуры в шейдере

Появляется `setTexture`. Тут возникает вопрос нумерации: у Metal индексы `[[texture(n)]]` и
`[[buffer(n)]]` **независимы**. Значит, текстура в слоте 0 и буфер в слоте 0 — разные вещи.

Решение — **отдельный дескрипторный набор**:

```
набор 0, привязки 0…7    буферы
набор 1, привязки 0…7    комбинированные сэмплеры изображений
```

В GLSL это выглядит как `layout(set = 1, binding = 0) uniform sampler2D`. Нумерация слотов в
API совпадает с обеими платформами, и приложение не думает про наборы.

### 15.7. Проверка

Проход 1 рисует сетку и «карту расстояний» в две оффскрин-текстуры. Проход 2 сводит их на
экран с делителем посередине. Переключатель «одна цель / две» показывает оба пути.

---

## Выпуск 16. Смешивание, глубина, растеризация

### 16.1. Что было захардкожено

Пайплайн создавался с фиксированными состояниями:

```cpp
colorBlendAttachment.blendEnable = VK_FALSE;          // никакой прозрачности
rasterizer.cullMode = VK_CULL_MODE_NONE;              // никакого отсечения
rasterizer.polygonMode = VK_POLYGON_MODE_FILL;        // никакого каркаса
depthStencil.depthCompareOp = VK_COMPARE_OP_LESS;     // не настраивается
depthStencil.stencilTestEnable = VK_FALSE;            // трафарета нет вовсе
```

Без смешивания невозможны прозрачность, частицы, декали, интерфейс поверх сцены — то есть
большая часть визуализации.

### 16.2. Описание состояний

Выносим в отдельный заголовок `PipelineState.hpp` перечисления и три структуры:

```cpp
struct BlendState {
    bool enabled = false;
    BlendFactor srcColorFactor = BlendFactor::One;
    BlendFactor dstColorFactor = BlendFactor::Zero;
    BlendOp colorOp = BlendOp::Add;
    BlendFactor srcAlphaFactor = BlendFactor::One;
    BlendFactor dstAlphaFactor = BlendFactor::Zero;
    BlendOp alphaOp = BlendOp::Add;
    ColorComponent writeMask = ColorComponent::All;

    static BlendState alphaBlend();   // src*a + dst*(1-a)
    static BlendState additive();     // src + dst
};
```

Именованные конструкторы для типовых случаев — приём, который сильно повышает читаемость места
использования. `BlendState::alphaBlend()` понятнее шести присваиваний.

Аналогично `DepthStencilState::depthTestAndWrite()` и `depthTestOnly()`. Второй — для
прозрачной геометрии: проверяем глубину, но не пишем, иначе прозрачные объекты перекрывают
друг друга неправильно.

### 16.3. Смешивание — по одному состоянию на цель

Vulkan требует запись на **каждое** вложение, даже если все одинаковы:

```cpp
std::vector<VkPipelineColorBlendAttachmentState> colorBlendAttachments(
    targetFormat.colorFormats.size());
for (size_t i = 0; i < colorBlendAttachments.size(); ++i) {
    const BlendState blend = desc.blendStates.empty() ? BlendState{} : desc.blendStates[i];
    // …
}
```

Пустой список означает «непрозрачно везде» — частый случай не должен требовать заполнения
вектора.

Побочная возможность: проход с несколькими целями может смешивать одну и перезаписывать
другую. Это нужно, например, когда в первую цель идёт накопление света, а во вторую — маска.

### 16.4. Собираем состояния в описание пайплайна

```cpp
struct PipelineDesc {
    std::shared_ptr<ShaderFunction> vertexFunction;
    std::shared_ptr<ShaderFunction> fragmentFunction;
    RenderTargetFormat targetFormat;
    std::vector<BlendState> blendStates;
    DepthStencilState depthStencil{};
    RasterizerState rasterizer{};
    std::string debugName;
};
```

Старую четырёхаргументную форму оставляем — она перенаправляет в описание со значениями по
умолчанию. Существующие места вызова не переписываются, а новая возможность доступна.

### 16.5. Асимметрия Metal — здесь она максимальна

Напомним из части II и доведём до конца:

| Состояние | Vulkan | Metal |
|---|---|---|
| Шейдеры, форматы, смешивание | `VkPipeline` | `MTLRenderPipelineState` |
| Глубина и трафарет | `VkPipeline` | **отдельный** `MTLDepthStencilState` |
| Отсечение, обход, заливка, смещение | `VkPipeline` | **команды энкодера** |
| Опорное значение трафарета | `VkPipeline` | **команда энкодера** |

`MetalPipeline` владеет двумя объектами, помнит третью группу и применяет всё в
`applyEncoderState()`:

```cpp
void MetalPipeline::applyEncoderState(void* encoder) const {
    auto mtlEncoder = (__bridge id<MTLRenderCommandEncoder>)encoder;
    [mtlEncoder setCullMode:m_data->m_cullMode];
    [mtlEncoder setFrontFacingWinding:m_data->m_winding];
    [mtlEncoder setTriangleFillMode:m_data->m_fillMode];
    [mtlEncoder setDepthBias:m_data->m_depthBiasConstant
                  slopeScale:m_data->m_depthBiasSlope clamp:0.0f];
    if (m_data->m_depthStencilState) [mtlEncoder setDepthStencilState:m_data->m_depthStencilState];
    if (m_data->m_stencilTestEnabled) [mtlEncoder setStencilReferenceValue:m_data->m_stencilReference];
}
```

Одна деталь: у Vulkan опорное значение трафарета своё для каждой стороны, у Metal — одно на
энкодер. Берём значение лицевой стороны и документируем.

Ещё: в Metal нет отдельного флага «тест глубины включён». Эквивалент выключенного теста —
функция сравнения `Always` и запрет записи.

### 16.6. Проверка на согласованность

Тест глубины требует цели глубины. Ловим внятно, а не даём драйверу упасть:

```cpp
if (!hasDepth && (desc.depthStencil.depthTestEnabled || desc.depthStencil.stencilTestEnabled)) {
    throw std::runtime_error(
        "запрошен тест глубины или трафарета, но depthFormat не задан");
}
```

### 16.7. Первый настоящий 3D-объект

Теперь можно нарисовать вращающийся куб: тест глубины, отсечение задних граней, матрица в
push-константах, цвет из объёмной текстуры вперемешку с кубической картой.

Важное следствие исправления из части VIII (система координат): матрица проекции пишется
**один раз** и работает на обеих платформах — ни переворота Y, ни пересчёта диапазона глубины.

```cpp
Mat4 perspective(float fovYRadians, float aspect, float nearZ, float farZ) {
    const float f = 1.0f / std::tan(fovYRadians * 0.5f);
    Mat4 m{};
    m[0] = f / aspect;
    m[5] = f;
    m[10] = farZ / (nearZ - farZ);
    m[11] = -1.0f;
    m[14] = (nearZ * farZ) / (nearZ - farZ);
    return m;
}
```

Глубина очищается в 1.0 — дальняя плоскость, чтобы любой фрагмент сначала прошёл тест.

### 16.8. Проверка

Куб вращается, ближние грани перекрывают дальние, внутренние поверхности отсечены. Полупрозрачный
оверлей поверх сцены не заменяет пиксели, а смешивается с ними.

---

## Выпуск 17. Сглаживание (MSAA)

### 17.1. Как это работает

Многосэмпловое изображение хранит несколько значений на пиксель. Растеризатор определяет
покрытие для каждого сэмпла отдельно, фрагментный шейдер при этом выполняется **один раз** на
пиксель. Затем сэмплы усредняются. Края становятся гладкими, стоимость шейдера почти не растёт.

### 17.2. Три ограничения и откуда они

```cpp
if (multisampled) {
    if (m_data->mipLevels != 1)
        throw std::runtime_error("у многосэмплового изображения не бывает мип-уровней");
    if (hasFlag(desc.usage, ImageUsage::Sampled))
        throw std::runtime_error(
            "многосэмпловое изображение нельзя сэмплить: сведите его в обычное");
}
```

Все три — свойства формата хранения, а не нашей библиотеки. Сообщения формулируем так, чтобы
из них было понятно, **что делать вместо**.

### 17.3. Сведение

Сведение (resolve) — усреднение сэмплов в обычное изображение. Ключевое: оно происходит **при
завершении прохода** и не стоит отдельной отрисовки.

```cpp
pass->setColorAttachment(0, msaaTarget, true, clearValue);
pass->setResolveAttachment(0, ordinaryTarget);
```

* **Metal**: свойство вложения — `resolveTexture` плюс операция сохранения
  `MTLStoreActionMultisampleResolve`.
* **Vulkan**: массив `pResolveAttachments` в подпроходе, параллельный массиву цветовых.

Порядок вложений в проходе становится: **цвета → цели сведения → глубина**. Список
представлений для фреймбуфера собирается в том же порядке.

### 17.4. Оптимизация, которую легко упустить

Многосэмпловое вложение, которое сводится, — чистый черновик: читается только результат
сведения. Значит, сами сэмплы можно **не сохранять**:

```cpp
description.storeOp = color.hasResolve ? VK_ATTACHMENT_STORE_OP_DONT_CARE
                                       : VK_ATTACHMENT_STORE_OP_STORE;
```

На мобильных GPU с тайловой архитектурой это позволяет вообще не выгружать сэмплы в память —
экономия огромная. На десктопе выигрыш меньше, но он бесплатный.

### 17.5. Число сэмплов — часть идентичности пайплайна

```cpp
multisampling.rasterizationSamples = ToVkSampleCount(targetFormat.sampleCount);
```

Отсюда неприятное практическое следствие: **включить сглаживание на лету нельзя**. Пайплайн,
собранный под один сэмпл, недействителен в проходе с четырьмя. Нужен отдельный пайплайн на
каждую конфигурацию — и это надо явно сказать в документации, иначе пользователь напорется.

В нашем демо это дало четыре варианта пайплайна плоскости: {одна цель, MRT} × {1×, 4×}.

### 17.6. Спрашивать, а не предполагать

```cpp
SampleCount VulkanDevice::maxSupportedSampleCount() const {
    const VkSampleCountFlags supported =
        m_data->properties.limits.framebufferColorSampleCounts &
        m_data->properties.limits.framebufferDepthSampleCounts;
    // …
}
```

Пересечение цветовых и глубинных возможностей — проход пишет и то, и другое с одинаковой
частотой, поэтому годятся только общие значения.

### 17.7. Проверка

```
[msaa] enabled=1 samples=4x max=8x
```

Плюс визуально: диагональные края куба при включённом сглаживании гладкие, при выключенном —
ступенчатые. Хороший кадр для сравнения в видео — увеличенный фрагмент края.

---

## Итог части VI

* сняты ограничения «одна цель» и «только свопчейн»; появились оффскрин-цели и MRT;
* кэш фреймбуферов переехал туда, где живёт его инвалидация; сформулировано общее правило;
* синхронизация между проходами оформлена внешними зависимостями — приложение по-прежнему не
  пишет барьеров;
* фиксированные состояния стали настраиваемыми; разобрана максимальная асимметрия Metal, где
  одно понятие «пайплайн» разложено на три механизма;
* добавлено сглаживание со сведением; показана оптимизация с отказом от сохранения сэмплов и
  ограничение «число сэмплов запекается в пайплайн».

Дальше — производительность.

**Следующая часть:** [Часть VII. Производительность](07-proizvoditelnost.ru.md)
