# Часть III. Геометрия и камера

> **Цель главы.** Загрузить настоящую модель, включить буфер глубины и разобраться с его
> точностью — так, чтобы Sponza с её диапазоном от сантиметра до сотни метров не разваливалась на
> мерцающие полигоны. И сделать камеру, которой можно управлять мышью, потому что без неё
> невозможно ни отладить сцену, ни показать её в кадре.

---

## 3.1. Индексный буфер и кеш вершин

Начнём с того, зачем вообще индексы. Куб из 12 треугольников без индексов — это 36 вершин, хотя
уникальных всего 8 (или 24, если у граней разные нормали). С индексами — 8 вершин и 36 индексов
по 4 байта. Экономия по памяти есть, но она не главное.

Главное — **кеш пост-трансформации** из главы 1. Аппаратура помнит результаты обработки
последних ~16–32 вершин, и при повторной ссылке на тот же индекс вершинный шейдер **не
запускается**. Без индексов такой возможности нет вовсе: каждая вершина уникальна по номеру, и
шейдер выполняется 36 раз вместо 8.

```cpp
std::shared_ptr<GBuffer> vertexBuffer = device->createBuffer(
    BufferType::Vertex, BufferUsage::Static,
    mesh.vertices.size() * sizeof(MeshVertex), mesh.vertices.data(), "Vertices");

std::shared_ptr<GBuffer> indexBuffer = device->createBuffer(
    BufferType::Index, BufferUsage::Static,
    mesh.indices.size() * sizeof(uint32_t), mesh.indices.data(), "Indices");
```

Отрисовка:

```cpp
cmd->setVertexBuffer(0, vertexBuffer);
cmd->drawIndexed(indexBuffer, IndexType::UInt32,
                 /*indexCount*/   subset.indexCount,
                 /*instanceCount*/1,
                 /*байтовое смещение первого индекса*/ subset.firstIndex * sizeof(uint32_t),
                 /*vertexOffset*/ 0,
                 /*firstInstance*/0);
```

Обратите внимание: пятый параметр — смещение **в байтах**, а не в индексах. Забыть умножить на
`sizeof(uint32_t)` — значит нарисовать не тот кусок модели. Симптом узнаваемый: часть геометрии
на месте, часть превратилась в кашу из длинных треугольников.

### UInt16 против UInt32

`IndexType::UInt16` вдвое меньше по памяти и лучше ложится в кеш. Ограничение — 65 535 вершин на
буфер. Для отдельного объекта этого хватает почти всегда; для сцены целиком — нет. Практичный
подход: хранить сцену в 32-битных индексах, а на этапе подготовки ассетов разбивать крупные
объекты на куски, помещающиеся в 16 бит. В курсе мы этим не занимаемся, но на San Miguel в главе
12 разница будет заметна, и стоит про неё знать.

### Порядок треугольников имеет значение

Кеш вершин попадает только если треугольники, делящие вершины, идут рядом в индексном буфере.
Загрузчики OBJ выдают треугольники в порядке файла, а он произволен.

Библиотека [`meshoptimizer`](https://github.com/zeux/meshoptimizer) переупорядочивает индексы
так, чтобы попадание в кеш было максимальным:

```cpp
meshopt_optimizeVertexCache(indices.data(), indices.data(),
                            indices.size(), vertexCount);
meshopt_optimizeOverdraw(/* ... */);       // порядок спереди назад
meshopt_optimizeVertexFetch(/* ... */);    // переупорядочить сами вершины
```

Третий вызов не менее полезен первого: он переставляет **вершины** так, чтобы читаемые подряд
индексы обращались к соседним участкам буфера. Это уже не кеш пост-трансформации, а обычный кеш
памяти, и на модели в миллионы вершин выигрыш измеримый. Мы вернёмся к этому в главе 12.

---

## 3.2. Буфер глубины

### Что он делает физически

Буфер глубины — это изображение того же размера, что и цветовая цель, в котором для каждого
пикселя хранится глубина ближайшего пока нарисованного фрагмента. При растеризации нового
фрагмента аппаратура сравнивает его глубину с сохранённой и решает, рисовать или выбросить.

Но реальность интереснее, и это важно для производительности:

**Hi-Z / иерархическая глубина.** GPU держит **уменьшенную** копию буфера глубины, где на каждый
блок (обычно 8×8 пикселей) записаны минимум и максимум глубины в блоке. Прежде чем растеризовать
треугольник, аппаратура сравнивает его диапазон глубины с этим блоком, и если весь треугольник
заведомо позади — **отбрасывает целый блок разом**, не порождая фрагментов вообще. Именно Hi-Z
делает сортировку спереди назад настолько эффективной.

**Сжатие буфера глубины.** Глубина внутри треугольника меняется линейно, поэтому вместо значений
можно хранить коэффициенты плоскости. Аппаратура делает это прозрачно, и трафик глубины на
практике заметно меньше наивного расчёта из главы 1.

Обе оптимизации отключаются, если шейдер пишет глубину сам. Ещё одна причина не делать этого без
крайней нужды.

### Создание и подключение

```cpp
constexpr ImageFormat kDepthFormat = ImageFormat::D32_FLOAT;

ImageDesc depthDesc{};
depthDesc.format    = kDepthFormat;
depthDesc.width     = swapChain->width();
depthDesc.height    = swapChain->height();
depthDesc.usage     = ImageUsage::DepthStencil;
depthDesc.debugName = "DepthTarget";
std::shared_ptr<GImage> depthTarget = device->createImage(depthDesc);
```

В проходе:

```cpp
pass->setColorAttachment(0, target, true, clearColor);
pass->setDepthStencilAttachment(depthTarget,
                                /*clearDepth*/  true,
                                /*depthValue*/  1.0f,
                                /*clearStencil*/false,
                                /*stencilValue*/0);
```

В пайплайне:

```cpp
desc.targetFormat = RenderTargetFormat::singleTarget(kColorFormat, kDepthFormat);
desc.depthStencil = DepthStencilState::depthTestAndWrite();
```

Три места. Пропуск любого из них — ошибка: формат глубины в `RenderTargetFormat` обязан совпадать
с форматом подключённого изображения, а `depthTestAndWrite()` бесполезен без вложения глубины в
проходе.

**Буфер глубины надо пересоздавать при изменении размера окна.** Он не часть свопчейна, и никто
не сделает этого за нас. Размер — от свопчейна, как обсуждалось в главе 2.

---

## 3.3. Точность глубины: почему Sponza мерцает

Вот проблема, с которой сталкивается каждый, кто первый раз грузит сцену из архива, и которую
почти никто не объясняет до конца.

### Откуда берётся неравномерность

Проекционная матрица делает не линейное преобразование глубины, а **гиперболическое**. После
деления на `w` глубина в буфере равна примерно

```
z_буфер ≈ (far / (far - near)) · (1 − near / z_вид)
```

То есть значение зависит от **обратной** величины расстояния. Половина всего диапазона буфера
уходит на первые `2·near` единиц.

Числа для `near = 0.1`, `far = 1000`:

| Расстояние | Значение в буфере | Занятая доля диапазона |
|---|---|---|
| 0.1 (near) | 0.0 | — |
| 0.2 | 0.500 | 50 % на первые 0.1 единицы |
| 1.0 | 0.900 | |
| 10 | 0.990 | |
| 100 | 0.999 | |
| 1000 (far) | 1.0 | 0.1 % на последние 900 единиц |

На расстоянии в сотню метров два объекта, разнесённых на десять сантиметров, попадают в одно и то
же значение буфера. Аппаратура не может решить, что ближе, и решение меняется от кадра к кадру в
зависимости от округления — получается мерцание, которое называют **Z-fighting**.

### Первое лекарство: отодвиньте ближнюю плоскость

Это самое действенное и самое недооценённое средство. Точность зависит от отношения `far/near`, и
`near` в этой формуле — **числитель проблемы**. Увеличение `near` с 0.01 до 0.1 улучшает точность
на дальних планах в десять раз. Уменьшение `far` с 10000 до 1000 — всего в десять раз меньше
эффекта на том же участке.

Практическое правило: **`near` — настолько большое, насколько терпимо**. Если камера не подходит
к геометрии ближе чем на полметра, ставьте `near = 0.5`, а не 0.01 «на всякий случай».

### Второе лекарство: reverse-Z

Приём, который в современных движках применяется по умолчанию, и наш API его полностью
поддерживает.

Идея. Числа с плавающей точкой распределены неравномерно: между 0 и 1 их гораздо больше, чем
между 0.5 и 1. Гиперболическое распределение глубины тоже неравномерно, но в **другую** сторону —
плотно у ближней плоскости. Если развернуть глубину так, чтобы ближняя плоскость давала 1, а
дальняя 0, две неравномерности почти взаимно компенсируются, и относительная точность становится
почти постоянной на всём диапазоне.

Нужны три согласованных изменения:

**1. Проекционная матрица отображает near → 1, far → 0:**

```cpp
Mat4 perspectiveReverseZ(float fovY, float aspect, float nearZ, float farZ)
{
    const float f = 1.0f / std::tan(fovY * 0.5f);
    Mat4 m{};
    m[0]  = f / aspect;
    m[5]  = f;
    m[10] = nearZ / (farZ - nearZ);
    m[11] = -1.0f;
    m[14] = (nearZ * farZ) / (farZ - nearZ);
    return m;
}
```

**2. Очищаем глубину нулём, а не единицей:**

```cpp
pass->setDepthStencilAttachment(depthTarget, /*clearDepth*/true, /*depthValue*/0.0f,
                                false, 0);
```

**3. Меняем функцию сравнения на «больше»:**

```cpp
DepthStencilState depth{};
depth.depthTestEnabled  = true;
depth.depthWriteEnabled = true;
depth.depthCompareOp    = CompareOp::Greater;   // было Less
desc.depthStencil = depth;
```

**Формат обязан быть `D32_FLOAT`.** С `D24_UNORM_S8_UINT` reverse-Z не даёт ничего: значения
там распределены равномерно, и компенсировать нечего. Это единственное жёсткое требование приёма.

Бонус, ради которого стоит потерпеть неудобство: при reverse-Z можно взять **бесконечную дальнюю
плоскость** и вообще перестать её настраивать:

```cpp
m[10] = 0.0f;
m[14] = nearZ;
```

Точность при этом почти не страдает, а из настроек камеры исчезает параметр, который всё равно
никто не умеет выставлять правильно.

Одно предупреждение: перейдя на reverse-Z, **не забудьте про все остальные места**, где
используется сравнение глубины. `DepthStencilState::depthTestOnly()` в обёртке использует
`LessOrEqual` — для reverse-Z нужен `GreaterOrEqual`. Это укусит в главе 9, на прозрачности.

### Третье лекарство: масштаб сцены

Sponza из архива смоделирована в **сантиметрах**: ширина атриума порядка 2000–3000 единиц. Если
поставить `near = 0.1`, отношение `far/near` окажется чудовищным.

Есть два подхода. Можно нормировать модель при загрузке — так делает наш `FastRenderer.cpp`,
приводя габарит к единице. Можно оставить исходный масштаб и подобрать `near`/`far` под него —
для Sponza это примерно `near = 1`, `far = 5000`.

Второй способ правильнее для книги: он сохраняет физические размеры, а они понадобятся, когда
дойдём до затухания света (глава 6) — интенсивность падает как обратный квадрат расстояния, и
если «метр» на самом деле сантиметр, все настройки света поедут в 10 000 раз.

Наш `Mesh` даёт всё необходимое для решения:

```cpp
const float extent = mesh.boundsExtent();   // габарит модели
const auto  center = mesh.center();

// Правдоподобные плоскости отсечения для сцены любого масштаба
const float nearZ = extent * 0.001f;
const float farZ  = extent * 10.0f;
```

---

## 3.4. Матрицы

Обёртка ничего не знает про матрицы — это код приложения. Разберём то, что нужно.

### Соглашение о хранении

Мы храним матрицы **по столбцам** (column-major), как в GLSL и MSL. Элемент строки `r` и столбца
`c` лежит по индексу `c * 4 + r`. Такая матрица передаётся в шейдер как `float4x4` без
транспонирования, и умножение записывается как `M * v`.

```cpp
using Mat4 = std::array<float, 16>;

Mat4 multiply(const Mat4& a, const Mat4& b)
{
    Mat4 r{};
    for (int c = 0; c < 4; ++c)
        for (int row = 0; row < 4; ++row) {
            float sum = 0.0f;
            for (int k = 0; k < 4; ++k) sum += a[k * 4 + row] * b[c * 4 + k];
            r[c * 4 + row] = sum;
        }
    return r;
}
```

Порядок аргументов: `multiply(projection, view)` означает «сначала view, потом projection», как
и в математической записи `P · V`.

### Матрица вида

```cpp
Mat4 lookAt(const Vec3& eye, const Vec3& target, const Vec3& up)
{
    const Vec3 f = normalize(target - eye);       // вперёд
    const Vec3 s = normalize(cross(f, up));       // вправо
    const Vec3 u = cross(s, f);                   // истинный «вверх»

    Mat4 m{};
    m[0] = s.x; m[4] = s.y; m[8]  = s.z;  m[12] = -dot(s, eye);
    m[1] = u.x; m[5] = u.y; m[9]  = u.z;  m[13] = -dot(u, eye);
    m[2] = -f.x; m[6] = -f.y; m[10] = -f.z; m[14] = dot(f, eye);
    m[15] = 1.0f;
    return m;
}
```

Минус перед `f` — потому что система правосторонняя и камера смотрит вдоль **отрицательного** Z.
Это соглашение согласовано с `perspective()`, где `m[11] = -1`.

### Матрица нормалей — и ловушка MSL

Нормаль нельзя преобразовывать той же матрицей, что и позицию. При неравномерном масштабировании
нормаль перестанет быть перпендикулярной поверхности. Правильное преобразование — **обратная
транспонированная** матрица модели.

Пока модель только вращается и масштабируется равномерно, обратная транспонированная совпадает с
обычной, и можно передавать нормаль как есть — так сделано в `Mesh.metal`. Как только появится
неравномерный масштаб, понадобится отдельная матрица.

**И вот ловушка: в MSL нет встроенной функции `inverse()` для `float4x4`.** Ни `inverse`, ни
`transpose` от обратной — ничего. Считать обращение матрицы в шейдере вручную — двадцать строк
арифметики на каждую вершину, при том что результат одинаков для всей модели.

Правильно — считать на CPU и передавать готовую. Матрица нормалей нужна только 3×3, но
push-константы придётся заполнять аккуратно: `float3x3` в MSL занимает **48 байт** (три колонки
по 16), а не 36.

```cpp
struct SceneConstants {
    float modelViewProjection[16];   // 64
    float normalMatrix[12];          // 48: три колонки по float4, w игнорируется
    float lightDirection[4];         // 16
};
static_assert(sizeof(SceneConstants) == 128, "ровно в гарантированный лимит");
```

```metal
struct SceneConstants {
    float4x4 modelViewProjection;
    float3x3 normalMatrix;      // 48 байт — совпадает
    float4   lightDirection;
};
```

128 байт — впритык. Как только понадобится больше, придётся переносить часть в uniform-буфер. В
главе 5 мы так и сделаем.

---

## 3.5. Камера, управляемая мышью

Две камеры покрывают все потребности курса: орбитальная для разглядывания модели и
«полётная» для прогулки по интерьеру. Sponza и San Miguel требуют второй.

### Состояние и обработка ввода

```cpp
struct Camera {
    Vec3  position { 0.0f, 1.6f, 3.0f };
    float yaw   = 0.0f;      // поворот вокруг оси Y, радианы
    float pitch = 0.0f;      // наклон, ограничен ±89°
    float fovY  = 1.0472f;   // 60 градусов
    float speed = 3.0f;      // единиц в секунду

    Vec3 forward() const {
        return { std::cos(pitch) * std::sin(yaw),
                 std::sin(pitch),
                 -std::cos(pitch) * std::cos(yaw) };
    }
    Vec3 right() const { return normalize(cross(forward(), Vec3{0, 1, 0})); }
    Mat4 view()  const { return lookAt(position, position + forward(), {0, 1, 0}); }
};
```

Обработка мыши. Ключевой момент — **не воровать ввод у интерфейса**:

```cpp
void updateCamera(Camera& camera, GLFWwindow* window, float deltaTime)
{
    const ImGuiIO& io = ImGui::GetIO();

    static double lastX = 0.0, lastY = 0.0;
    static bool   dragging = false;

    double mouseX, mouseY;
    glfwGetCursorPos(window, &mouseX, &mouseY);

    const bool pressed = glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_RIGHT) == GLFW_PRESS;

    // Если курсор над окном ImGui — вращать камеру нельзя.
    if (pressed && !io.WantCaptureMouse) {
        if (!dragging) { lastX = mouseX; lastY = mouseY; dragging = true; }

        const float sensitivity = 0.0025f;
        camera.yaw   += static_cast<float>(mouseX - lastX) * sensitivity;
        camera.pitch -= static_cast<float>(mouseY - lastY) * sensitivity;

        // Ограничение наклона: ровно ±90° вырождает матрицу вида,
        // потому что forward() становится коллинеарен «вверх».
        camera.pitch = std::clamp(camera.pitch, -1.553f, 1.553f);
    } else {
        dragging = false;
    }
    lastX = mouseX; lastY = mouseY;

    if (io.WantCaptureKeyboard) return;

    const float step = camera.speed * deltaTime *
                       (glfwGetKey(window, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS ? 5.0f : 1.0f);

    if (glfwGetKey(window, GLFW_KEY_W) == GLFW_PRESS) camera.position += camera.forward() * step;
    if (glfwGetKey(window, GLFW_KEY_S) == GLFW_PRESS) camera.position -= camera.forward() * step;
    if (glfwGetKey(window, GLFW_KEY_D) == GLFW_PRESS) camera.position += camera.right()   * step;
    if (glfwGetKey(window, GLFW_KEY_A) == GLFW_PRESS) camera.position -= camera.right()   * step;
    if (glfwGetKey(window, GLFW_KEY_E) == GLFW_PRESS) camera.position.y += step;
    if (glfwGetKey(window, GLFW_KEY_Q) == GLFW_PRESS) camera.position.y -= step;
}
```

Три детали, каждая из которых была бы багом:

**`io.WantCaptureMouse`.** Без этой проверки перетаскивание ползунка в ImGui одновременно
вращает камеру. ImGui выставляет флаг, когда курсор над его окном или он захватил ввод.

**Ограничение наклона.** При ровно ±90° вектор «вперёд» становится коллинеарен «вверх», векторное
произведение обращается в ноль, и матрица вида вырождается. Камера скачком переворачивается.
Ограничиваем чуть меньшим значением (1.553 рад ≈ 89°).

**`deltaTime` в скорости, но не в повороте.** Перемещение — это скорость, её надо умножать на
время. Поворот мышью — это уже смещение, оно от времени не зависит. Умножить поворот на
`deltaTime` — значит сделать чувствительность мыши зависящей от частоты кадров. Ошибка частая и
неприятная.

**Скорость надо масштабировать под сцену.** 3 единицы в секунду разумно для сцены габаритом в
десяток единиц и абсурдно для Sponza в сантиметрах. Привяжите к габаритам:
`camera.speed = mesh.boundsExtent() * 0.5f`.

### Про deltaTime

```cpp
double lastTime = glfwGetTime();
while (!glfwWindowShouldClose(window)) {
    const double now = glfwGetTime();
    const float deltaTime = static_cast<float>(now - lastTime);
    lastTime = now;
    ...
}
```

Этого достаточно для камеры, но **недостаточно для анимации**. Причина в том, что кадры
показываются по темпу монитора, а `glfwGetTime()` измеряет время на процессоре, который идёт
впереди на два кадра. Отсюда микродрожание, заметное на равномерном движении. Подробно — в главе
10; пока просто знайте, что для камеры, управляемой человеком, это несущественно, а для
автоматического вращения — очень даже.

---

## 3.6. Отсечение задних граней

```cpp
desc.rasterizer.cullMode  = CullMode::Back;
desc.rasterizer.frontFace = FrontFace::CounterClockwise;
```

Даёт бесплатно вдвое меньше фрагментов на замкнутой геометрии: половина треугольников любого
замкнутого тела смотрит от нас. Отбрасывание происходит **до** растеризации, так что экономятся и
фрагменты, и работа с глубиной.

Обёртка выровняла clip-пространство между бэкендами, поэтому одна и та же настройка даёт один и
тот же результат на Mac и на Windows — обычно это место, где два API расходятся.

Но с ассетами архива есть практическая сложность: **в них полно геометрии с неправильным или
неопределённым порядком обхода**. Листва и ткань часто смоделированы односторонними
плоскостями, которые предполагается видеть с обеих сторон. Если включить отсечение глобально,
часть листьев Sponza исчезнет при взгляде с изнанки.

Правильное решение — **отсечение как свойство материала**, а не глобальная настройка:

```cpp
// Два пайплайна, выбираемые по материалу
std::shared_ptr<Pipeline> opaqueCulled  = makePipeline(CullMode::Back);
std::shared_ptr<Pipeline> opaqueTwoSided = makePipeline(CullMode::None);
```

Как определить, какие материалы двусторонние? В MTL прямого признака нет. Работающая эвристика:
**материал с альфа-маской (`map_d`) считаем двусторонним**. Она покрывает листву и ткань, то есть
ровно те случаи, где это нужно. К этому вернёмся в главе 9.

Для двусторонней геометрии нормаль надо разворачивать во фрагментном шейдере, иначе изнанка будет
освещена неправильно:

```metal
fragment float4 mesh_fragment(VertexOut in [[stage_in]],
                              bool frontFacing [[front_facing]])
{
    float3 normal = normalize(in.worldNormal);
    if (!frontFacing) normal = -normal;
    ...
}
```

---

## 3.7. Собираем главу вместе

Кадр целиком:

```cpp
while (!glfwWindowShouldClose(window)) {
    glfwPollEvents();

    const double now = glfwGetTime();
    const float deltaTime = static_cast<float>(now - lastTime);
    lastTime = now;

    std::shared_ptr<GImage> target = swapChain->acquireNextImage();
    if (!target) continue;

    const int width  = static_cast<int>(swapChain->width());
    const int height = static_cast<int>(swapChain->height());
    if (width == 0 || height == 0) continue;
    ensureTargets(width, height);          // пересоздаёт глубину при изменении размера

    updateCamera(camera, window, deltaTime);

    const float aspect = static_cast<float>(width) / static_cast<float>(height);
    const Mat4 projection = perspectiveReverseZ(camera.fovY, aspect, nearZ, farZ);
    const Mat4 viewProjection = multiply(projection, camera.view());

    SceneConstants constants{};
    const Mat4 mvp = multiply(viewProjection, modelMatrix);
    std::copy(mvp.begin(), mvp.end(), constants.modelViewProjection);

    std::shared_ptr<CommandBuffer> cmd = helper::createCommandBuffer(queue);

    std::shared_ptr<RenderPassDescriptor> pass = helper::createRenderPassDescriptor();
    pass->setColorAttachment(0, target, true, clearColor);
    pass->setDepthStencilAttachment(depthTarget, true, /*reverse-Z*/0.0f, false, 0);

    cmd->beginRenderPass(pass);
    cmd->setRenderPipeline(pipeline);
    cmd->setVertexBuffer(0, vertexBuffer);
    for (const MeshSubset& subset : mesh.subsets) {
        cmd->setPushConstants(ShaderStage::Vertex,   &constants, sizeof(constants));
        cmd->setPushConstants(ShaderStage::Fragment, &constants, sizeof(constants));
        cmd->drawIndexed(indexBuffer, IndexType::UInt32, subset.indexCount, 1,
                         subset.firstIndex * sizeof(uint32_t), 0, 0);
    }
    cmd->endRenderPass();

    cmd->present(target);
    cmd->commit();
}
```

---

## 3.8. Что обычно ломается

| Симптом | Причина |
|---|---|
| Модель видна «изнутри», ближнее рисуется поверх дальнего | Не подключено вложение глубины, или в пайплайне нет `depthStencil` |
| Ничего не видно после включения reverse-Z | Забыли одно из трёх: матрицу, очистку в 0.0, `CompareOp::Greater` |
| Мерцающие полигоны на дальнем плане | Слишком маленький `near`. Начните с него, не с `far` |
| reverse-Z не помог | Формат глубины не `D32_FLOAT` |
| Часть модели пропала | Неверное смещение в `drawIndexed`: индексы, а не байты |
| Часть модели пропала при `CullMode::Back` | Порядок обхода вершин в ассете; для этого материала нужен `CullMode::None` |
| Камера крутится при перетаскивании ползунка | Не проверяется `io.WantCaptureMouse` |
| Камера переворачивается при взгляде вверх | Не ограничен `pitch` |
| Чувствительность мыши зависит от FPS | `deltaTime` умножен на поворот, а не только на перемещение |
| Изнанка двусторонней геометрии освещена неверно | Не развёрнута нормаль по `[[front_facing]]` |

---

## 3.9. Проверка

1. **Модель отображается корректно с любого ракурса**, ближние части закрывают дальние. Буфер
   глубины работает.
2. **Поставьте `near = 0.001`, `far = 10000`** и подойдите камерой вплотную к дальней стене. При
   обычном Z будет мерцание; включите reverse-Z — оно исчезнет, при тех же плоскостях. Это
   прямое доказательство того, что приём работает, а не «стало вроде получше».
3. **Наведите курсор на окно ImGui и потяните правой кнопкой.** Камера не должна шевельнуться.
4. **Измерьте FPS с `CullMode::None` и `CullMode::Back`** на замкнутой модели. Разница должна быть
   заметна, если упираетесь во фрагментный шейдер.

---

## Итог

1. Индексы нужны не ради памяти, а ради **кеша пост-трансформации**. Порядок треугольников
   влияет на производительность; `meshoptimizer` это чинит.
2. Буфер глубины даёт не только корректность, но и **Hi-Z** — отбрасывание блоками до
   растеризации. Запись глубины из шейдера убивает и то, и другое.
3. Точность глубины гиперболична. **`near` важнее `far`**. Reverse-Z (матрица + очистка нулём +
   `CompareOp::Greater` + `D32_FLOAT`) практически снимает проблему.
4. Матрицы храним по столбцам. **В MSL нет `inverse()` для `float4x4`** — матрицу нормалей
   считаем на CPU.
5. Камера: проверяйте `io.WantCaptureMouse`, ограничивайте `pitch`, умножайте на `deltaTime`
   только перемещение.
6. Отсечение граней — свойство **материала**, а не сцены.

Дальше — [Часть IV. Материалы и текстуры](04-tekstury-materialy.ru.md): загрузчик изображений,
sRGB, мипы и первая по-настоящему текстурированная Sponza.
