# Часть V. Текстуры

**Выпуски 12–14.** До сих пор изображения можно было только создать пустыми и рисовать в них.
Научимся загружать пиксели, строить мип-цепочки, работать с объёмами, кубическими картами и
массивами, а затем сожмём текстуры блочными форматами и получим четырёхкратную экономию памяти.

---

## Выпуск 12. Загрузка текстур и мип-уровни

### 12.1. Что мешало раньше

Функция создания изображения не принимала данных вообще. В неё можно было только рисовать. Это
означало, что **никакую текстуру с диска показать нельзя** — довольно фундаментальная дыра для
графической библиотеки.

Второе: `mipLevels` всегда был равен единице, а `maxLod` сэмплера — нулю. Даже если бы мипы
существовали, читался бы только нулевой уровень.

### 12.2. Переход к структуре описания

Список параметров создания изображения перерос позиционные аргументы. Переходим на структуру:

```cpp
struct ImageDesc {
    ImageType type = ImageType::Image2D;
    ImageFormat format = ImageFormat::Undefined;
    uint32_t width = 1;
    uint32_t height = 1;
    uint32_t depth = 1;
    uint32_t arrayLayers = 1;
    uint32_t mipLevels = 1;
    SampleCount sampleCount = SampleCount::One;
    ImageUsage usage = ImageUsage::Sampled;
    std::string debugName;
};

virtual std::shared_ptr<GImage> createImage(const ImageDesc& desc,
                                            const void* initialData = nullptr) = 0;
```

Приём общего назначения: когда у функции больше четырёх-пяти параметров и у большинства есть
разумные значения по умолчанию, структура с именованными полями читается лучше ряда аргументов
и не ломается при добавлении новых.

### 12.3. Загрузка — это переходы состояний

Сама копия проста, сложность — в состояниях изображения вокруг неё:

```
1. Перевести уровень в состояние приёмника копирования
      старое состояние UNDEFINED — содержимое всё равно перезаписываем целиком
2. Скопировать из промежуточного буфера
3. Перевести уровень в состояние покоя
```

```cpp
transitionLevels(cmd, image, mipLevel, 1,
                 VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                 VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                 arrayLayer, 1);
```

Указание `UNDEFINED` как старого состояния — не лень, а оптимизация: драйвер получает
разрешение **не распаковывать** прежнее содержимое, которое мы всё равно затрём.

### 12.4. Дополнительные флаги использования

Загрузка требует флага «приёмник копирования», а построение мипов — ещё и «источник». Ни то,
ни другое не выражается через `ImageUsage`, и заставлять приложение о них помнить неправильно:

```cpp
imageInfo.usage |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
if (m_data->mipLevels > 1) {
    imageInfo.usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
}
```

### 12.5. Мип-уровни: зачем

Мип-цепочка — заранее уменьшенные копии. Без неё мелко показанная текстура превращается в шум:
пиксель экрана накрывает десятки текселей, а прочитан будет один, и какой именно — зависит от
дрожания камеры.

Число уровней:

```cpp
uint32_t resolveMipLevels(uint32_t requested, uint32_t width, uint32_t height, uint32_t depth) {
    const uint32_t largest = std::max(std::max(width, height), depth);
    uint32_t maximum = 1;
    while ((largest >> (maximum - 1)) > 1) ++maximum;
    if (requested == kFullMipChain) return maximum;
    return std::min(requested, maximum);
}
```

Для 256×256 получается девять уровней: 256, 128, 64, 32, 16, 8, 4, 2, 1.

### 12.6. Построение цепочки

Каждый уровень получается уменьшением предыдущего. Лестница переходов:

```cpp
// Нулевой уровень уже заполнен и лежит в состоянии покоя — переводим в источник.
transitionLevels(cmd, image, 0, 1, finalLayout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, …);

for (uint32_t level = 1; level < mipLevels; ++level) {
    transitionLevels(cmd, image, level, 1, VK_IMAGE_LAYOUT_UNDEFINED,
                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, …);

    VkImageBlit blit{};
    blit.srcOffsets[1] = { levelWidth, levelHeight, levelDepth };
    blit.srcSubresource.mipLevel = level - 1;
    blit.dstOffsets[1] = { nextWidth, nextHeight, nextDepth };
    blit.dstSubresource.mipLevel = level;

    vkCmdBlitImage(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                        image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                        1, &blit, VK_FILTER_LINEAR);

    // Этот уровень становится источником для следующего.
    transitionLevels(cmd, image, level, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                     VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, …);

    levelWidth = nextWidth; levelHeight = nextHeight; levelDepth = nextDepth;
}

// Вся цепочка сейчас источник — возвращаем в состояние покоя одним переходом.
transitionLevels(cmd, image, 0, mipLevels, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, finalLayout, …);
```

Металл делает это одной строкой: `[blit generateMipmapsForTexture:texture]`.

Формат обязан поддерживать линейную фильтрацию при копировании — проверяем и сообщаем внятно:

```cpp
if (!(formatProperties.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT)) {
    throw std::runtime_error("формат не поддерживает линейную фильтрацию при копировании");
}
```

### 12.7. Сэмплер должен разрешить чтение мипов

Мипы бесполезны, если сэмплер их не читает:

```cpp
info.mipmapMode = (desc.mipFilter == SamplerFilter::Nearest)
    ? VK_SAMPLER_MIPMAP_MODE_NEAREST : VK_SAMPLER_MIPMAP_MODE_LINEAR;
info.minLod = desc.minLod;
info.maxLod = desc.maxLod;     // раньше здесь был ноль
```

### 12.8. Проверка

Текстура 256×256, девять уровней, в видеопамяти. Рисуем её **мельче**, чем она хранится:
с мипами гладко, без них — муар. Наглядный кадр для видео: показать оба варианта рядом.

---

## Выпуск 13. Объёмы, кубические карты и массивы

### 13.1. Что было сломано

`ImageType` содержал `Image3D` и `CubeMap` с самого начала. Но:

* `depth` в описании изображения **не было** — глубина была захардкожена единицей;
* `arrayLayers` тоже не было, а загрузка всегда писала слой ноль.

То есть трёхмерное изображение получалось глубиной в один воксель, а у кубической карты
заполнялась одна грань из шести. **Значения перечисления, которые не работают** — худший вид
дыры: выглядит как поддержка, пока не попробуешь.

Урок для курса: перечисление — это обещание. Значение, которое нельзя использовать, стоит либо
реализовать, либо убрать.

### 13.2. Три разные вещи

Их легко перепутать:

| | Что это | Как адресуется | Пример |
|---|---|---|---|
| **3D** | Один объём | Третьей координатой текстуры | Плотность, таблица цветов |
| **Массив** | N независимых картинок | Индексом слоя | Каскады теней, атлас |
| **Куб** | Массив ровно из 6 | Направлением | Окружение, отражения |

Куб — частный случай массива, и это упрощает реализацию: почти весь код общий.

```cpp
m_data->arrayLayers = (desc.type == ImageType::CubeMap) ? 6 : std::max(1u, desc.arrayLayers);
m_data->depth = (desc.type == ImageType::Image3D) ? std::max(1u, desc.depth) : 1;

if (m_data->depth > 1 && m_data->arrayLayers > 1) {
    throw std::runtime_error("массивы объёмов не поддерживаются");
}
```

### 13.3. Тип представления

«Массивность» — часть типа представления, а не отдельный флаг:

```cpp
VkImageViewType toVkImageViewType(ImageType type, uint32_t arrayLayers) {
    switch (type) {
        case ImageType::Image3D: return VK_IMAGE_VIEW_TYPE_3D;   // объём не бывает массивом
        case ImageType::CubeMap: return VK_IMAGE_VIEW_TYPE_CUBE;
        case ImageType::Image2D:
        default: return arrayLayers > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
    }
}
```

В Metal — то же самое через `MTLTextureType2DArray` / `MTLTextureTypeCube`. Тонкость: у куба
`arrayLength` остаётся единицей, шесть граней подразумеваются типом.

### 13.4. Что покрывает один вызов загрузки

Договорённость, которую нужно проговорить явно:

```
2D             весь уровень,  w × h
3D             весь объём,    w × h × d, срезы подряд
Массив / куб   один слой,     w × h — вызывать по разу на слой
```

Грани куба идут в порядке `+X, −X, +Y, −Y, +Z, −Z`.

Размер считает общая функция:

```cpp
size_t VulkanImage::levelByteSize(uint32_t mipLevel) const {
    return imageLevelBytes(m_data->format,
                           std::max(1u, m_data->width  >> mipLevel),
                           std::max(1u, m_data->height >> mipLevel),
                           std::max(1u, m_data->depth  >> mipLevel));
}
```

### 13.5. Рендеринг в слой

Чтобы заполнить грань куба или срез каскада, проходу нужно указать слой:

```cpp
pass->setColorAttachment(0, cubeMap, true, clearValue, /*arrayLayer=*/faceIndex);
```

Здесь два API расходятся сильнее всего:

* **Metal** — свойство вложения: `attachment.slice = arrayLayer;`
* **Vulkan** — нужно **представление одного слоя**, потому что вложение фреймбуфера это
  представление, а не изображение.

```cpp
VkImageView VulkanImage::layerView(uint32_t arrayLayer) {
    if (m_data->arrayLayers <= 1) return m_data->imageView;
    if (auto it = m_data->layerViews.find(arrayLayer); it != m_data->layerViews.end())
        return it->second;

    VkImageViewCreateInfo viewInfo{};
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;   // один слой — обычная 2D-цель
    viewInfo.subresourceRange.baseArrayLayer = arrayLayer;
    viewInfo.subresourceRange.layerCount = 1;
    // создать и закэшировать
}
```

Представления кэшируются по слою и уничтожаются вместе с изображением — не забыв сообщить
устройству, чтобы оно выбросило фреймбуферы, которые их держат.

### 13.6. Мипы для объёмов и массивов

Два отличия от плоского случая:

* у объёма цепочка уменьшается **в трёх измерениях**;
* у массива один блит покрывает **все слои сразу** — они уменьшаются одинаково.

```cpp
const int32_t nextDepth = levelDepth > 1 ? levelDepth / 2 : 1;
blit.srcSubresource.layerCount = arrayLayers;
blit.dstSubresource.layerCount = arrayLayers;
```

### 13.7. Проверка через чтение обратно

Визуально проверить, что все шесть граней загрузились, тяжело. Читаем обратно и сравниваем:

```
[cube] face 0: rgb(220, 60, 60) OK      face 3: rgb( 30, 90, 30) OK
[cube] face 1: rgb( 90, 30, 30) OK      face 4: rgb( 60, 60,220) OK
[cube] face 2: rgb( 60,220, 60) OK      face 5: rgb( 30, 30, 90) OK
[volume] 32x32x32 round-trip OK; blue at z=0,16,31: 0,131,255
```

Приём, который стоит показать: **делать тестовые данные различимыми по всем осям**. Синий канал
объёма меняется вдоль Z — значения 0, 131, 255 доказывают, что глубина настоящая, а не
схлопнутая в один срез. Плоская заливка такой ошибки не покажет.

---

## Выпуск 14. Блочное сжатие

### 14.1. Зачем

Комплект текстур для реальной сцены не помещается в видеопамять несжатым. BC хранит блок
4×4 текселя в фиксированном числе байт: **выигрыш в 4–8 раз**, а распаковка при чтении
бесплатна — она встроена в текстурный блок GPU.

| Формат | Бит/тексель | Для чего |
|---|---|---|
| BC1 | 4 | Цвет, альфа 1 бит |
| BC3 | 8 | Цвет с плавной альфой |
| BC4 | 4 | Один канал: высоты, маски |
| BC5 | 8 | Два канала: нормали |
| BC7 | 8 | Максимальное качество |

Варианты `_SRGB` преобразуют при чтении из sRGB в линейное — это то, что нужно цветовым
текстурам. Данные, не являющиеся цветом (нормали, шероховатость), должны использовать `_UNORM`,
иначе получат неверную кривую.

### 14.2. Что ломается в существующем коде

Функция «байт на пиксель» перестаёт иметь смысл. Мы честно записали это ограничение, когда её
писали:

> Каждый формат, который выставляет абстракция, несжатый… добавление блочно-сжатого формата
> сделает эту функцию недостаточной.

Момент настал. Заменяем описанием блока:

```cpp
struct FormatInfo {
    uint32_t blockWidth = 1;    // 1 у несжатых, 4 у BC
    uint32_t blockHeight = 1;
    uint32_t blockBytes = 0;
    bool compressed = false;
};
```

Несжатый формат — блок 1×1, поэтому путь один, а не два:

```cpp
inline size_t imageLevelBytes(ImageFormat format, uint32_t width, uint32_t height, uint32_t depth) {
    const FormatInfo info = formatInfo(format);
    const uint32_t blocksX = (width + info.blockWidth - 1) / info.blockWidth;
    const uint32_t blocksY = (height + info.blockHeight - 1) / info.blockHeight;
    return static_cast<size_t>(blocksX) * blocksY * depth * info.blockBytes;
}
```

Округление **вверх до целых блоков** — уровень 1×1 у BC3 стоит полные 16 байт.

### 14.3. Шаг строки — где сжатие кусается

Самое коварное место. В Metal при копировании задаётся «байт на строку», и раньше он считался
как `size / height`. Для BC это **вчетверо больше правильного**: строка блоков покрывает
четыре строки текселей.

```cpp
inline size_t rowPitch(ImageFormat format, uint32_t width) {
    const FormatInfo info = formatInfo(format);
    const uint32_t blocksX = (width + info.blockWidth - 1) / info.blockWidth;
    return static_cast<size_t>(blocksX) * info.blockBytes;
}
```

Ошибка не диагностируется валидацией — она даёт мусор в текстуре. Хороший сюжет для видео:
показать, как выглядит картинка с неправильным шагом строки.

### 14.4. Три ограничения, вытекающие из природы блоков

```cpp
if (isCompressedFormat(desc.format)) {
    if (hasFlag(desc.usage, ImageUsage::ColorTarget) ||
        hasFlag(desc.usage, ImageUsage::DepthStencil) ||
        hasFlag(desc.usage, ImageUsage::Storage)) {
        throw std::runtime_error("в сжатое изображение нельзя рисовать");
    }
    if (desc.sampleCount != SampleCount::One) {
        throw std::runtime_error("сжатое изображение не может быть многосэмпловым");
    }
}
```

И третье, самое практичное: **мипы нельзя построить на GPU**. Копирование с фильтрацией не
умеет работать с блоками. Сжатая текстура приходит с уже готовой цепочкой.

```cpp
if (isCompressedFormat(m_data->format)) {
    throw std::runtime_error(
        "уровни сжатого изображения должны загружаться уже сжатыми, по одному update() на уровень");
}
```

### 14.5. Возможность устройства

BC требует включения:

```cpp
deviceFeatures.textureCompressionBC = supportedFeatures.textureCompressionBC;
```

Тот же шаблон «спросить, затем запросить поддерживаемое», что и в части I.

### 14.6. Простой кодировщик BC3

Чтобы демонстрация была настоящей, а не декларативной, напишем кодировщик. Реальные проекты
сжимают заранее отдельным инструментом, но понять формат полезно.

BC3 хранит блок 4×4 в 16 байтах: 8 на альфу и 8 на цвет.

**Альфа** — две конечные точки плюс трёхбитные индексы:

```cpp
void encodeAlphaBlock(const uint8_t alpha[16], uint8_t out[8]) {
    uint8_t minA = 255, maxA = 0;
    for (int i = 0; i < 16; ++i) { minA = std::min(minA, alpha[i]); maxA = std::max(maxA, alpha[i]); }

    out[0] = maxA;  out[1] = minA;

    uint64_t indices = 0;
    const int range = maxA - minA;
    for (int i = 0; i < 16; ++i) {
        uint32_t index = 0;
        if (range > 0) {
            const int t = ((alpha[i] - minA) * 7 + range / 2) / range;
            // Порядок индексов не 0..7: 0 — максимум, 1 — минимум, далее 2..7 по убыванию.
            index = (t == 7) ? 0u : (t == 0 ? 1u : static_cast<uint32_t>(8 - t));
        }
        indices |= static_cast<uint64_t>(index & 0x7) << (3 * i);
    }
    for (int i = 0; i < 6; ++i) out[2 + i] = static_cast<uint8_t>((indices >> (8 * i)) & 0xFF);
}
```

**Цвет** — две точки в RGB565 и двухбитные индексы. Проецируем каждый тексель на ось между
точками:

```cpp
const float t = std::clamp(dot / axisLengthSq, 0.0f, 1.0f);
const int step = static_cast<int>(t * 3.0f + 0.5f);
static const uint32_t stepToIndex[4] = { 1, 3, 2, 0 };
index = stepToIndex[step];
```

Конечные точки берём из ограничивающего параллелепипеда блока. Настоящий кодировщик вписывает
прямую в облако цветов — в этом почти вся разница в качестве.

### 14.7. Мипы сжимаем сами

Строим цепочку на CPU и сжимаем каждый уровень:

```cpp
for (uint32_t level = 0; level < texture->mipLevels(); ++level) {
    const std::vector<uint8_t> blocks = compressBC3(levelPixels, levelWidth, levelHeight);
    texture->update(blocks.data(), blocks.size(), level);

    if (level + 1 < texture->mipLevels()) {
        levelPixels = downsampleHalf(levelPixels, levelWidth, levelHeight);
        levelWidth = std::max(1u, levelWidth / 2);
        levelHeight = std::max(1u, levelHeight / 2);
    }
}
```

### 14.8. Проверка

```
BC3 256x256, 9 mip levels; 349524 -> 87408 bytes (4.00x)
level 0 = 65536 bytes (expect 65536), 1x1 level = 16 bytes (expect 16)
```

Ровно 4.00× — сходится с теорией (8 бит на тексель против 32). Уровень 1×1 в полные 16 байт
подтверждает округление до целых блоков.

Переключатель в панели: сжато / не сжато. Картинка почти не отличается, память вчетверо
меньше — лучший аргумент в пользу сжатия.

---

## Итог части V

* добавлена загрузка пикселей и построение мип-цепочек; сэмплер научился их читать;
* «мёртвые» значения перечисления превращены в работающие: объёмы, кубы и массивы, плюс
  рендеринг в отдельный слой;
* введено блочное сжатие; попутно заменена изначально неверная модель «байт на пиксель» на
  описание блока;
* показана ловушка шага строки, которую не ловит валидация;
* показан приём проверки через чтение обратно с данными, различимыми по всем осям.

Дальше — то, что делает картинку не плоской: проходы, состояния, глубина и сглаживание.

**Следующая часть:** [Часть VI. Проходы и состояния](06-prohody-i-sostoyaniya.ru.md)
