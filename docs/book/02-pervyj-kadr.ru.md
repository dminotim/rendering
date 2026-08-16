# Часть II. Первый кадр

**Выпуски 4–6.** К концу части на экране появится процедурная сетка — полноэкранный
прямоугольник, окрашенный фрагментным шейдером. По дороге разберём шейдеры, пайплайн, проход
рендеринга и — самое интересное — придумаем модель привязки ресурсов, которая одинаково ложится
на оба API.

---

## Выпуск 4. Шейдеры

### 4.1. Фундаментальное различие

| | Vulkan | Metal |
|---|---|---|
| Язык | GLSL (или другой) → **SPIR-V** | MSL |
| Когда компилируется | Заранее, при сборке | В рантайме, из строки |
| Что загружает рантайм | Готовый двоичный модуль | Исходный текст |
| Точка входа | Всегда `main` | Любое имя, выбирается при загрузке |

Metal умеет то, чего Vulkan не умеет вовсе: скомпилировать исходник во время работы и достать
из него функцию по имени. В Vulkan компилятора в рантайме просто нет.

### 4.2. Проектируем единый вызов

Хочется, чтобы в коде приложения было так — и на обеих платформах одинаково:

```cpp
auto vs = helper::createShaderFunction(device, path, "plane_vertex_shader");
auto fs = helper::createShaderFunction(device, path, "plane_fragment_shader");
```

Решение: путь передаётся **без расширения**, а бэкенд достраивает своё соглашение.

* **Metal** дописывает `.metal`, читает файл, компилирует, достаёт функцию по имени.
* **Vulkan** ищет `<путь>.<имя_функции>.spv` — по одному файлу на точку входа.

```cpp
std::filesystem::path resolveSpirvPath(const std::filesystem::path& base,
                                       const std::string& functionName) {
    std::filesystem::path resolved = base;
    resolved.replace_filename(base.stem().string() + "." + functionName + ".spv");
    return resolved;
}
```

Так `SHADER_DIR/PlaneShader` + `plane_vertex_shader` превращается в
`PlaneShader.plane_vertex_shader.spv`. Имя точки входа в SPIR-V при этом всегда `main` —
логическое имя живёт в имени файла.

### 4.3. Сборка шейдеров

Компиляция встраивается в CMake, чтобы шейдер нельзя было забыть пересобрать:

```cmake
set(GLSL_SHADERS
        "PlaneShader.vert:plane_vertex_shader:PlaneShader"
        "PlaneShader.frag:plane_fragment_shader:PlaneShader"
)

foreach(_entry ${GLSL_SHADERS})
    string(REPLACE ":" ";" _parts "${_entry}")
    list(GET _parts 0 _file)
    list(GET _parts 1 _function)
    list(GET _parts 2 _stem)

    add_custom_command(
            OUTPUT "${SHADER_DIR}/${_stem}.${_function}.spv"
            COMMAND "${GLSLC_EXECUTABLE}" --target-env=vulkan1.2
                    -o "${SHADER_DIR}/${_stem}.${_function}.spv"
                    "${CMAKE_SOURCE_DIR}/shaders/glsl/${_file}"
            DEPENDS "${CMAKE_SOURCE_DIR}/shaders/glsl/${_file}"
            VERBATIM
    )
endforeach()
```

Третье поле нужно потому, что несколько GLSL-файлов могут соответствовать точкам входа
**одного** файла Metal: `PlaneShader.metal` содержит и обычный, и MRT-вариант фрагментного
шейдера, а в GLSL это два отдельных файла.

Артефакты кладём в каталог сборки, а не в исходники: `SHADER_DIR` на Windows указывает на
`${CMAKE_BINARY_DIR}/shaders`, на macOS — на исходники, так как компиляция там в рантайме.

### 4.4. Проверка

Собираем — рядом с исполняемым файлом появились `.spv`. Пока их никто не использует.

---

## Выпуск 5. Пайплайн и проход рендеринга

### 5.1. Что такое пайплайн

Неизменяемый объект, который фиксирует **всё**: шейдеры, форматы целей, смешивание, глубину,
растеризацию. Создаётся дорого, привязывается дёшево. Причина в железе: смена состояний
требует перекомпиляции внутреннего кода, и API вынуждает сделать её явной и заранее.

Здесь ключевая асимметрия:

* **Vulkan** запекает всё в один `VkPipeline`.
* **Metal** разносит: смешивание и форматы — в `MTLRenderPipelineState`, глубина и трафарет —
  в **отдельный** `MTLDepthStencilState`, а отсечение, порядок обхода и смещение глубины —
  вообще **команды энкодера**.

Значит, `MetalPipeline` обязан владеть двумя объектами и **помнить** третью группу, а
`setRenderPipeline()` — применять всё сразу:

```cpp
void MetalCommandBuffer::setRenderPipeline(std::shared_ptr<Pipeline> pipeline) {
    auto mtlPipeline = (__bridge id<MTLRenderPipelineState>)pipeline->nativeHandle();
    [m_data->m_encoder setRenderPipelineState:mtlPipeline];

    auto* metalPipeline = static_cast<MetalPipeline*>(pipeline.get());
    metalPipeline->applyEncoderState((__bridge void*)m_data->m_encoder);
}
```

Иначе привязка пайплайна означала бы на двух платформах разное — и это именно тот тип
расхождения, ради устранения которого пишется обёртка.

### 5.2. Проход рендеринга: объект против значения

Второе крупное расхождение.

* **Metal**: `MTLRenderPassDescriptor` — значение, создаётся каждый кадр, бесплатно.
* **Vulkan**: `VkRenderPass` — объект, описывающий форматы и операции загрузки/сохранения,
  плюс `VkFramebuffer`, связывающий его с конкретными изображениями.

Форма API берётся от Metal:

```cpp
auto pass = helper::createRenderPassDescriptor();   // без аргументов!
pass->setColorAttachment(0, image, true, clearColor);
```

Но фабрика **не принимает устройство**, а Vulkan без устройства ничего создать не может.
Решение: `VulkanRenderPassDescriptor` — чистый объект-значение, который ничего не создаёт.
Разрешение в настоящие объекты происходит в `beginRenderPass()`, где устройство уже есть.

### 5.3. Кэширование

Создавать `VkRenderPass` каждый кадр расточительно. Кэшируем — но важно **где**.

```
Проходы рендеринга → кэш на устройстве
    зависят только от форматов и операций загрузки;
    переживают изменение размера окна; общие для всех пайплайнов

Фреймбуферы → кэш на устройстве, но с инвалидацией
    ссылаются на конкретные представления изображений;
    обязаны умереть вместе с ними
```

Тот, кто уничтожает представление изображения, обязан сначала попросить устройство выбросить
фреймбуферы, которые на него ссылаются:

```cpp
for (VkImageView view : m_data->imageViews) {
    m_data->device->invalidateFramebuffersUsing(view);
    vkDestroyImageView(logicalDevice, view, nullptr);
}
```

### 5.4. Совместимость проходов

Тонкость, которая экономит огромное количество объектов: в Vulkan проходы **совместимы**, если
совпадают количество вложений, форматы и число сэмплов. Операции загрузки и сохранения на
совместимость **не влияют**.

Практический вывод: пайплайн можно собрать против варианта «с очисткой» и спокойно использовать
внутри варианта «с загрузкой». Один пайплайн — оба случая.

### 5.5. Ключ кэша

```cpp
struct RenderPassAttachmentKey {
    VkFormat format;
    bool clear;
    VkImageLayout restingLayout;
    bool hasResolve;
    VkImageLayout resolveRestingLayout;
};

struct RenderPassKey {
    std::vector<RenderPassAttachmentKey> colors;
    RenderPassAttachmentKey depth;
    VkSampleCountFlagBits samples;
};
```

Структура вырастет в последующих частях (сведение MSAA, глубина), но принцип задаётся сейчас.

### 5.6. Понятие «состояние покоя» изображения

Vulkan требует, чтобы изображение находилось в подходящем состоянии для каждой операции.
Полноценный трекинг состояний — отдельная подсистема, и она нам не нужна, потому что API
предоставляет ровно два перехода: «записано проходом» и «прочитано сэмплером».

Поэтому каждому изображению приписывается **одно** состояние покоя, выводимое из его
назначения:

```cpp
VkImageLayout RestingLayoutFor(ImageUsage usage, bool isSwapChainImage) {
    if (isSwapChainImage) return VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    if (hasFlag(usage, ImageUsage::DepthStencil))
        return VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    if (hasFlag(usage, ImageUsage::Sampled))
        return VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    return VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
}
```

Каждый проход **оставляет** вложения в состоянии покоя и, если не очищает их, **ожидает**
найти их в нём. Это и есть причина, по которой приложение никогда не пишет барьеров: они
сводятся к описанию прохода.

### 5.7. Динамический вьюпорт

Metal задаёт вьюпорт на весь аттачмент сам. Vulkan — нет. Чтобы поведение совпало и чтобы
изменение размера не требовало пересборки пайплайна, объявляем вьюпорт динамическим и задаём
в `beginRenderPass()`.

Здесь же — решение, которое станет критичным в части VIII: **высота вьюпорта отрицательная**.
Пока запомним, что так сделано; разбор — там, где оно понадобится.

### 5.8. Проверка

Экран заливается цветом очистки. Первый настоящий результат.

---

## Выпуск 6. Буферы и модель привязки

### 6.1. Проблема, которой нет в Metal

Metal читает геометрию так:

```metal
vertex VertexOut plane_vertex_shader(
        const device VertexData* vertex_array [[buffer(0)]],
        uint vertex_id [[vertex_id]])
{
    out.position = float4(vertex_array[vertex_id].position, 0.0, 1.0);
}
```

Никакого описания вершинных атрибутов. Просто указатель на буфер и индекс.

Vulkan традиционно требует `VkVertexInputAttributeDescription` — формальное описание раскладки
вершины. Но наш `createPipeline()` его **не принимает**, потому что Metal он не нужен.

Три варианта:

1. Расширить публичный API описанием вершинного формата — но тогда он существует ради одного
   бэкенда.
2. Выводить раскладку рефлексией SPIR-V — новая зависимость, много кода.
3. **Сделать так же, как Metal**: читать геометрию из буфера по индексу вершины.

Выбираем третий. В GLSL прямой эквивалент — storage-буфер, индексируемый `gl_VertexIndex`:

```glsl
struct VertexData { vec2 position; };

layout(std430, set = 0, binding = 0) readonly buffer VertexBuffer {
    VertexData vertices[];
} vertexBuffer;

void main() {
    gl_Position = vec4(vertexBuffer.vertices[gl_VertexIndex].position, 0.0, 1.0);
}
```

Пайплайн объявляет **ноль** вершинных привязок и ноль атрибутов. Публичный API не меняется, а
шейдеры становятся дословными переводами друг друга.

Цена решения честная: раскладку вершины задаёт структура в шейдере, а не описание при создании
пайплайна. Произвольную модель придётся приводить к единому формату при загрузке. Для курса
это приемлемо, и в части VIII мы вернёмся к этому в списке ограничений.

### 6.2. Модель слотов

Из этого решения естественно рождается правило:

```
номер слота == номер привязки в шейдере
```

Ровно как индекс `[[buffer(n)]]` в Metal.

```
дескрипторный набор 0 (буферы):
    binding 0        storage-буфер, вершинная стадия   ← setVertexBuffer(0, …)
    binding 1…7      uniform-буфер, обе стадии         ← setUniformBuffer(n, …)
```

Все слоты объявляются в раскладке **независимо от того, использует ли их шейдер**. Один
`VkDescriptorSetLayout` обслуживает любую пару шейдеров, соблюдающую соглашение; незаполненные
привязки просто не записываются.

Побочный эффект, полезный на практике: два вызова `setUniformBuffer` для одного слота — с
`ShaderStage::Vertex` и `ShaderStage::Fragment` — схлопываются в **одну** запись дескриптора,
потому что одна привязка Vulkan охватывает обе стадии. В Metal пара действительно нужна; здесь
второй вызов идемпотентен.

### 6.3. Отложенная привязка

Дескрипторный набор нельзя создать, не зная раскладку, а раскладка известна только после
`setRenderPipeline()`. Значит, `setVertexBuffer` и `setUniformBuffer` только **запоминают**:

```cpp
void VulkanCommandBuffer::setUniformBuffer(uint32_t slot, ShaderStage stage,
                                           const std::shared_ptr<GBuffer>& buffer, size_t offset) {
    auto* vulkanBuffer = static_cast<VulkanBuffer*>(buffer.get());
    m_data->bindings[slot] = BoundBuffer{
        vulkanBuffer,
        vulkanBuffer->currentRegionOffset() + offset,
        VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER
    };
}
```

Настоящий набор собирается перед отрисовкой. В части VII мы сделаем это ещё и быстрым.

### 6.4. Раскладка констант — источник тихих багов

Структура в C++:

```cpp
struct Uniforms {
    float viewportSize[2];   // 0, 4
    float scale;             // 8
    float pan[2];            // 12, 16
};
```

Очевидный перевод в GLSL **неверен**:

```glsl
// НЕПРАВИЛЬНО
layout(std140, binding = 1) uniform Uniforms {
    vec2 viewportSize;   // 0
    float scale;         // 8
    vec2 pan;            // 16 !!! ожидалось 12
};
```

`vec2` в `std140` выравнивается по 8 байт, и `pan` уезжает. Шейдер прочитает `panX` из ячейки
`panY`, картинка «поедет», и никакой диагностики не будет.

Надёжное решение — **скалярные поля**:

```glsl
layout(std140, binding = 1) uniform Uniforms {
    float viewportSizeX;   // 0
    float viewportSizeY;   // 4
    float scale;           // 8
    float panX;            // 12
    float panY;            // 16
};
```

Скаляр в `std140` выравнивается по 4 байта и укладывается вплотную. Раскладка совпадает
побайтово, без расширений и без паддинга.

**Это стоит отдельного эпизода в видео.** Ошибка выравнивания — самый частый источник
«необъяснимо неправильной» картинки, и она не диагностируется ничем.

### 6.5. Индексная отрисовка

Тонкость совместимости: Metal задаёт начало отрисовки **байтовым** смещением в индексном
буфере, Vulkan — **номером индекса**. Чтобы поведение совпало точно, включая смещения, не
кратные размеру индекса, сворачиваем байтовое смещение в привязку буфера:

```cpp
vkCmdBindIndexBuffer(cmd, indexBuffer->buffer(),
                     indexBuffer->currentRegionOffset() + firstIndexOffsetBytes,
                     vkIndexType);
vkCmdDrawIndexed(cmd, indexCount, instanceCount, 0, vertexOffset, firstInstance);
```

### 6.6. Фрагментный шейдер сетки

```glsl
void main() {
    vec2 screen = gl_FragCoord.xy;
    vec2 world = screen - vec2(uniforms.panX, uniforms.panY);

    const float cellSizePx = 50.0;
    const float lineWidthPx = 2.0;

    float worldCellSize = cellSizePx * uniforms.scale;
    vec2 cell = fract(world / worldCellSize) * worldCellSize;

    bool isLine = (cell.x < lineWidthPx) || (cell.y < lineWidthPx);

    outColor = isLine ? vec4(0.60, 0.75, 0.95, 1.0)
                      : vec4(0.98, 0.98, 0.96, 1.0);
}
```

`gl_FragCoord` и `[[position]]` во фрагментной функции Metal совпадают полностью: пиксельные
координаты, начало в левом верхнем углу, выборка в центрах пикселей. Поэтому этот шейдер
переносится **дословно**.

### 6.7. Проверка

Сетка на экране. Ползунки масштаба и сдвига (пока хардкодом) её двигают.

---

## Итог части II

* шейдеры собираются заранее и находятся по логическому имени точки входа — одинаково на обеих
  платформах;
* пайплайн, проход и кэш проходов на месте; понятие «состояние покоя» устраняет барьеры из
  кода приложения;
* придумана модель привязки «номер слота = номер привязки», при которой публичный API не
  обзавёлся описанием вершинных атрибутов;
* разобрана ловушка выравнивания `std140`, которую ничем не поймать, кроме понимания.

Код пока пронизан Vulkan насквозь. Следующая часть — вытаскивание абстракции.

**Следующая часть:** [Часть III. Абстракция и второй бэкенд](03-abstrakciya.ru.md)
