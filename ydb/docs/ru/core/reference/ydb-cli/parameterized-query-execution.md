# Выполнение параметризованных запросов

## Обзор

{{ ydb-short-name }} CLI поддерживает исполнение [параметризованных запросов](https://en.wikipedia.org/wiki/Prepared_statement). Для работы с параметрами в тексте запроса должны присутствовать их определения [командой YQL `DECLARE`](../../yql/reference/syntax/declare.md).

Основной инструмент для выполнения параметризованных запросов в {{ ydb-short-name }} CLI — это команда [{{ ydb-cli }} sql](sql.md).

## Зачем использовать параметризованные запросы?

Использование параметризованных запросов предоставляет несколько важных преимуществ:

* **Улучшенная производительность:** Параметризованные запросы значительно повышают производительность при выполнении множества схожих запросов, которые различаются только входными параметрами. Это достигается с помощью [подготовленных запросов](https://en.wikipedia.org/wiki/Prepared_statement). Запрос компилируется один раз и затем кешируется на сервере. Последующие запросы с тем же текстом минуют фазу компиляции и сразу начинают выполняться.

* **Защита от SQL-инъекций:** Другим важным преимуществом использования параметризованных запросов является защита от [SQL-инъекций](https://en.wikipedia.org/wiki/SQL_injection). Эта функция безопасности гарантирует правильную обработку входных данных, что снижает риск выполнения вредоносного кода.

## Единичное исполнение запроса {#one-request}

Эта команда поддерживает передачу параметров через опции командной строки, файл, а также через `stdin`. При передаче параметров через `stdin` или файл поддерживается многократное поточное исполнение запроса с разными значениями параметров и возможностью пакетирования. Для этого в команде [{{ ydb-cli }} sql](sql.md) предназначены следующие параметры:

| Имя | Описание |
| ---|--- |
| `-p, --param` | Значение одного параметра запроса в формате `$name=value` или `name=value`, где `name` — имя параметра, а `value` — его значение (корректный [JSON value](https://www.json.org/json-ru.html)). |
| `--input-file` | Имя файла в формате [JSON](https://ru.wikipedia.org/wiki/JSON) в кодировке [UTF-8](https://ru.wikipedia.org/wiki/UTF-8), в котором заданы значения параметров, сопоставляемые с параметрами запроса по именам ключей. Может быть использован максимум один файл с параметрами. |
| `--input-format` | Формат представления значений параметров. Действует на все способы их передачи (через параметр команды, файл или `stdin`).<br/>Возможные значения:<ul><li>`json` (по умолчанию): Формат [JSON](https://ru.wikipedia.org/wiki/JSON).</li><li>`csv` — формат [CSV](https://ru.wikipedia.org/wiki/CSV). По умолчанию имена параметров должны находиться в header'е CSV файла. При единичном исполнении запроса допустима только одна строка в файле, не считая header'a.</li><li>`tsv` — формат [TSV](https://ru.wikipedia.org/wiki/TSV).</li><li>`raw`: Входной поток из `stdin` или `--input-file` содержит только значение параметра в виде бинарных данных. Имя параметра должно быть указано опцией `--input-param-name`.</li></ul> |
| `--input-binary-strings` | Формат кодировния значения параметров с типом «бинарная строка» (`DECLARE $par AS String`). Говорит о том, как должны интерпретироваться бинарные строки из входного потока.<br/>Возможные значения:<ul><li>`unicode`: Каждый байт в бинарной строке, не являющийся печатаемым ASCII-символом (codes 32-126), должен быть закодирован в кодировке [UTF-8](https://ru.wikipedia.org/wiki/UTF-8).</li><li>`base64`: Бинарные строки представлены в кодировке [Base64](https://ru.wikipedia.org/wiki/Base64). Такая возможность позволяет передавать бинарные данные, декодирование которых из Base64 выполнит {{ ydb-short-name }} CLI.</li></ul> |

Если значения указаны для всех параметров, которые не допускают `NULL` (то есть с типом NOT NULL), [в составе оператора `DECLARE`](../../yql/reference/syntax/declare.md), запрос будет выполнен на сервере. Если отсутствует значение хотя бы для одного такого параметра, выполнение команды завершится с ошибкой и сообщением "Не задано значение для параметра".

### Более специфичные опции для использования входных параметров {#specific-param-options}

Следующие опции не представлены в выводе `--help`. Их описание можно увидеть только в выводе `-hh`.

| Имя | Описание |
| ---|--- |
| `--input-framing` | Задает фрейминг для входного потока (`stdin` или `--input-file`). Определяет как входной поток будет разделяться на отдельные наборы параметров.<br/>Возможные значения:<br/><ul><li>`no-framing` (по умолчанию): От входного потока ожидается один набор параметров, запрос исполняется однократно.</li><li>`newline-delimited`: Символ перевода строки отмечает во входном потоке окончание одного набора параметров, отделяя его от следующего. Набор параметров считается собранным каждый раз при получении во входном потоке символа перевода строки.</li></ul> |
| `--input-param-name` | Имя параметра, значение которого передано во входной поток. Указывается без символа `$`. Обязательно при использовании формата `raw` в `--input-format`.<br/><br/>При использовании с JSON-форматом входной поток интерпретируется не как JSON-документ, а как JSON value, с передачей значения в параметр с указанным именем. |
| `--input-columns` | Строка с именами колонок, заменяющими header CSV/TSV документа, читаемого из входного потока. При указании опции считается, что header отсутствует. Опция допустима только с форматами CSV и TSV.|
| `--input-skip-rows` | Число строк с начала данных, читаемых со stdin'a, которые нужно пропустить, не включая строку header'a, если она имеется. Опция допустима только с форматами stdin'a CSV и TSV. |
| `--input-batch` | Режим пакетирования значений наборов параметров, получаемых из входного потока (`stdin` или `--input-file`).<br/>Возможные значения:<br/><ul><li>`iterative` (по умолчанию): Пакетирование [выключено](#streaming-iterate). Запрос выполняется для каждого набора параметров (ровно один раз, если в опции `--input-framing` используется `no-framing`)</li><li>`full`: Полный пакет. Запрос выполнится один раз после завершения чтения входного потока, все полученные наборы параметров заворачиваются в `List<...>`, имя параметра задается опцией `--input-param-name`</li><li>`adaptive`: Адаптивное пакетирование. Запрос выполняется каждый раз, когда срабатывает ограничение на количество наборов параметров в одном запросе (`--input-batch-max-rows`) или на задержку обработки (`--input-batch-max-delay`). Все полученные к этому моменту наборы параметров заворачиваются в `List<...>`, имя параметра задается опцией `--input-param-name`.</li></ul> |
| `--input-batch-max-rows` | Максимальное количество наборов параметров в пакете для адаптивного режима пакетирования. Следующий пакет будет отправлен на исполнение вместе с запросом, если количество наборов данных в нем достигло указанного значения. Установка в `0` снимает ограничение.<br/><br/>Значение по умолчанию — `1000`.<br/><br/>Параметры передаются в запрос без стриминга и общий объем одного GRPC-запроса, в который включаются значения параметров, имеет верхнюю границу около 5 МБ. |
| `--input-batch-max-delay` | Максимальная задержка отправки на обработку полученного набора параметров для адаптивного режима пакетирования. Задается в виде числа с размерностью времени - `s` (секунды), `ms` (миллисекунды), `m` (минуты) и т.д. Значение по умолчанию — `1s` (1 секунда).<br/><br/>{{ ydb-short-name }} CLI будет отсчитывать время с момента получения первого набора параметров для пакета и отправит накопившийся пакет на исполнение, как только время превысит указанное значение. Параметр позволяет получить эффективное пакетирование в случае непредсказуемого темпа появления новых наборов параметров на `stdin`. |

### Примеры {#examples-one-request}

{% include [ydb-cli-profile](../../_includes/ydb-cli-profile.md) %}

#### Передача значения одного параметра {#example-simple}

В командной строке, через опцию `--param`:

```bash
{{ ydb-cli }} -p quickstart sql -s 'DECLARE $a AS Int64; SELECT $a' --param '$a=10'
```

Через файл в формате JSON (который используется по умолчанию):

```bash
echo '{"a":10}' > p1.json
{{ ydb-cli }} -p quickstart sql -s 'DECLARE $a AS Int64; SELECT $a' --input-file p1.json
```

Через `stdin`, передавая JSON-строку как набор из одного параметра:

```bash
echo '{"a":10}' | {{ ydb-cli }} -p quickstart sql -s 'DECLARE $a AS Int64; SELECT $a'
```

Через `stdin`, передавая только значение параметра и задавая имя параметра через опцию `--input-param-name`:

```bash
echo '10' | {{ ydb-cli }} -p quickstart sql -s 'DECLARE $a AS Int64; SELECT $a' --input-param-name a
```

#### Передача значений параметров разных типов из нескольких источников {#example-multisource}

```bash
# Create a JSON file with fields 'a', 'b', and 'x', where 'x' will be ignored in the query
echo '{ "a":10, "b":"Some text", "x":"Ignore me" }' > p1.json

# Run the query using ydb-cli, passing in 'a' and 'b' from the input file, and 'c' as a direct parameter
{{ ydb-cli }} -p quickstart sql \
  -s 'DECLARE $a AS Int64;
      DECLARE $b AS Utf8;
      DECLARE $c AS Int64;

      SELECT $a, $b, $c' \
  --input-file p1.json \
  --param '$c=30'
```

Вывод команды:

```text
┌─────────┬─────────────┬─────────┐
│ column0 │ column1     │ column2 │
├─────────┼─────────────┼─────────┤
│ 10      │ "Some text" │ 30      │
└─────────┴─────────────┴─────────┘
```

#### Передача бинарных строк в кодировке Base64 {#example-base64}

```bash
{{ ydb-cli }} -p quickstart sql \
  -s 'DECLARE $a AS String;
      SELECT $a' \
  --input-format json \
  --input-binary-strings base64 \
  --param '$a="SGVsbG8sIHdvcmxkCg=="'
```

Вывод команды:

```text
┌──────────────────┐
| column0          |
├──────────────────┤
| "Hello, world\n" |
└──────────────────┘
```

#### Прямая передача бинарного контента {#example-raw}

```bash
curl -Ls http://ydb.tech/docs/en | {{ ydb-cli }} -p quickstart sql \
  -s 'DECLARE $a AS String;
      SELECT LEN($a)' \
  --input-format raw \
  --input-param-name a
```

Вывод команды (точное количество байт может отличаться):

```text
┌─────────┐
| column0 |
├─────────┤
| 66426   |
└─────────┘
```

#### Передача файла в формате CSV {#example-csv}

```bash
echo '10,Some text' | {{ ydb-cli }} -p quickstart sql \
  -s 'DECLARE $a AS Int32;
      DECLARE $b AS String;
      SELECT $a, $b' \
  --input-format csv \
  --input-columns 'a,b'
```

Вывод команды:

```text
┌─────────┬─────────────┐
| column0 | column1     |
├─────────┼─────────────┤
| 10      | "Some text" |
└─────────┴─────────────┘
```

## Итеративная потоковая обработка {#streaming-iterate}

{{ ydb-short-name }} CLI поддерживает возможность многократного исполнения запроса с разными наборами значений параметров, при их передаче через `stdin` **или** входной файл (не одновременно). При этом соединение с базой данных устанавливается однократно, а план исполнения запроса кешируется, что существенно повышает производительность такого подхода по сравнению с отдельными вызовами CLI.

Для того чтобы воспользоваться этой возможностью, необходимо на вход команды друг за другом передавать разные наборы значений одних и тех параметров, сообщив {{ ydb-short-name }} CLI правило, по которому будет возможно отделить эти наборы друг от друга.

Запрос выполняется столько раз, сколько наборов значений параметров было получено. Каждый полученный через входной поток (`stdin` или `--input-file`) набор объединяется со значениями параметров, определенными через опции `--param`. Исполнение команды будет завершено после завершения входного потока. Каждый запрос исполняется в своей транзакции.

Правило отделения наборов параметров друг от друга (фрейминг) дополняет описание формата представления параметров на входном потоке, задаваемое параметром `--input--framing`:

| Имя | Описание |
| ---|--- |
| `--input-framing` | Задает фрейминг для входного потока (файл или `stdin`). <br/>Возможные значения:<ul><li>`no-framing` (по умолчанию) — на входе ожидается один набор параметров, запрос исполняется однократно после завершения чтения потока.</li><li>`newline-delimited` — символ перевода строки отмечает на входе окончание одного набора параметров, отделяя его от следующего. Запрос исполняется каждый раз при получении символа перевода строки.</li></ul> |

{% note warning %}

При использовании символа перевода строки в качестве разделителя наборов параметров необходимо гарантировать его отсутствие внутри наборов параметров. Заключение текста в кавычки не делает допустимым перевод строки внутри такого текста. Многострочные JSON-документы не допускаются.

{% endnote %}

### Пример {#example-streaming-iterate}

#### Потоковая обработка нескольких наборов параметров {#example-iterate}

{% list tabs %}

- JSON

  Допустим, нам необходимо выполнить запрос трижды, со следующими наборами значений параметров `a` и `b`:

  1. `a` = 10, `b` = 20
  2. `a` = 15, `b` = 25
  3. `a` = 35, `b` = 48

  Создадим файл, который будет содержать строки с JSON-представлением этих наборов:

  ```bash
  echo -e '{"a":10,"b":20}\n{"a":15,"b":25}\n{"a":35,"b":48}' | tee par1.txt
  ```

  Вывод команды:

  ```text
  {"a":10,"b":20}
  {"a":15,"b":25}
  {"a":35,"b":48}
  ```

  Исполним запрос, передав на `stdin` содержимое данного файла, с форматированием вывода в JSON:

  ```bash
  cat par1.txt | \
  {{ ydb-cli }} -p quickstart sql \
    -s 'DECLARE $a AS Int64;
        DECLARE $b AS Int64;
        SELECT $a+$b' \
    --input-framing newline-delimited \
    --format json-unicode
  ```

  Вывод команды:

  ```text
  {"column0":30}
  {"column0":40}
  {"column0":83}
  ```

  Или просто передав имя файла в опцию `--input-file`:

  ```bash
  {{ ydb-cli }} -p quickstart sql \
    -s 'DECLARE $a AS Int64;
        DECLARE $b AS Int64;
        SELECT $a + $b' \
    --input-file par1.txt \
    --input-framing newline-delimited \
    --format json-unicode
  ```

  Вывод команды:

  ```text
  {"column0":30}
  {"column0":40}
  {"column0":83}
  ```

  Полученный в таком формате результат можно использовать для передачи на вход команде исполнения следующего запроса.

- CSV

  Допустим, нам необходимо выполнить запрос трижды, со следующими наборами значений параметров `a` и `b`:

  1. `a` = 10, `b` = 20
  2. `a` = 15, `b` = 25
  3. `a` = 35, `b` = 48

  Создадим файл c наборами значений параметров в формате CSV:

  ```bash
  echo -e 'a,b\n10,20\n15,25\n35,48' | tee par1.txt
  ```

  Вывод команды:

  ```text
  a,b
  10,20
  15,25
  35,48
  ```

  Исполним запрос, передав на `stdin` содержимое данного файла, с форматированием вывода в CSV:

  ```bash
  cat par1.txt | \
  {{ ydb-cli }} -p quickstart sql \
    -s 'DECLARE $a AS Int64;
        DECLARE $b AS Int64;
        SELECT $a + $b' \
    --input-format csv \
    --input-framing newline-delimited \
    --format csv
  ```

  Вывод команды:

  ```text
  30
  40
  83
  ```

  Или просто передав имя файла в опцию `--input-file`:

  ```bash
  {{ ydb-cli }} -p quickstart sql \
    -s 'DECLARE $a AS Int64;
        DECLARE $b AS Int64;
        SELECT $a + $b' \
    --input-file par1.txt \
    --input-format csv \
    --input-framing newline-delimited \
    --format csv
  ```

  Вывод команды:

  ```text
  30
  40
  83
  ```

  Полученный в таком формате результат можно использовать для передачи на вход команде исполнения следующего запроса, задав header данным в CSV формате опцией `--input-columns`.

- TSV

  Допустим, нам необходимо выполнить запрос трижды, со следующими наборами значений параметров `a` и `b`:

  1. `a` = 10, `b` = row1
  2. `a` = 15, `b` = row  2
  3. `a` = 35, `b` = "row"\n3

  Создадим файл c наборами значений параметров в формате TSV:

  ```bash
  echo -e 'a\tb\n10\t20\n15\t25\n35\t48' | tee par1.txt
  ```

  Вывод команды:

  ```text
  a  b
  10 20
  15 25
  35 48
  ```

  Исполним запрос, передав на `stdin` содержимое данного файла, с форматированием вывода в TSV:

  ```bash
  cat par1.txt | \
  {{ ydb-cli }} -p quickstart sql \
    -s 'DECLARE $a AS Int64;
        DECLARE $b AS Utf8;
        SELECT $a, $b' \
    --input-framing newline-delimited \
    --input-format tsv \
    --format tsv
  ```

  Вывод команды:

  ```text
  30
  40
  83
  ```

  Или просто передав имя файла в опцию `--input-file`:

  ```bash
  {{ ydb-cli }} -p quickstart sql \
    -s 'DECLARE $a AS Int64;
        DECLARE $b AS Int64;
        SELECT $a + $b' \
    --input-file par1.txt \
    --input-format tsv \
    --input-framing newline-delimited \
    --format tsv
  ```

  Вывод команды:

  ```text
  30
  40
  83
  ```

  Полученный в таком формате результат можно использовать для передачи на вход команде исполнения следующего запроса, задав header данным в TSV формате опцией `--input-columns`.

{% endlist %}

#### Потоковая обработка с объединением значений параметров из разных источников {#example-iterate-union}

Допустим, нам необходимо выполнить запрос трижды, со следующими наборами значений параметров `a` и `b`:

1. `a` = 10, `b` = 100
2. `a` = 15, `b` = 100
3. `a` = 35, `b` = 100

```bash
echo -e '10\n15\n35' | \
{{ ydb-cli }} -p quickstart sql \
  -s 'DECLARE $a AS Int64;
      DECLARE $b AS Int64;
      SELECT $a + $b AS sum1' \
  --param '$b=100' \
  --input-framing newline-delimited \
  --input-param-name a \
  --format json-unicode
```

Вывод команды:

```text
{"sum1":110}
{"sum1":115}
{"sum1":135}
```

## Пакетная потоковая обработка {#streaming-batch}

{{ ydb-short-name }} CLI поддерживает автоматическую конвертацию наборов параметров в `List<...>`, позволяя одним запросом к серверу обработать множество наборов параметров в одной транзакции, что может дополнительно существенно повышать производительность по сравнению с выполнением запросов "по одному".

Поддерживаются два режима пакетирования:

- Полный (`full`);
- Адаптивный (`adaptive`).

### Полный режим пакетирования {#batch-full}

Полный (`full`) режим является упрощенным вариантом пакетирования, когда запрос выполняется один раз, с заворачиванием в `List<...>` всех полученных с входного потока наборов параметров. Если размер запроса окажется слишком большим, будет выдана ошибка.

Данный вариант пакетирования необходим в случае, когда необходимо гарантировать атомарность за счет применения всех параметров в одной транзакции.

### Адаптивный режим пакетирования {#batch-adaptive}

При работе в адаптивном (`adaptive`) режиме обработка входного потока разбивается на множество транзакций, с автоматическим подбором размера пакета для каждой из них.

Данный режим позволяет эффективно обрабатывать широкий спектр входной нагрузки, с непредсказуемым или бесконечным количеством данных, а также непредсказуемым или сильно меняющимся темпом их появления на входе. В частности, такой профиль характерен при подаче на `stdin` выхода другой команды через оператор `|`.

Адаптивный режим решает две основные проблемы обработки динамического потока:

1. Ограничение максимального размера пакета.
2. Ограничение максимальной задержки обработки данных.

### Синтаксис {#batch-syntax}

Для того чтобы воспользоваться возможностью пакетирования, необходимо описать параметр типа `List<...>` или `List<Struct<...>>` в секции DECLARE запроса, и выбрать режим следующим параметром:

| Имя | Описание |
| ---|--- |
| `--input-batch` | Режим пакетирования значений наборов параметров, получаемых из входного потока (`stdin` или `--input-file`).<br/>Возможные значения:<br/><ul><li>`iterative` (по умолчанию): Пакетирование [выключено](#streaming-iterate). Запрос выполняется для каждого набора параметров (ровно один раз, если в опции `--input-framing` используется `no-framing`)</li><li>`full`: Полный пакет. Запрос выполнится один раз после завершения чтения входного потока, все полученные наборы параметров заворачиваются в `List<...>`, имя параметра задается опцией `--input-param-name`</li><li>`adaptive`: Адаптивное пакетирование. Запрос выполняется каждый раз, когда срабатывает ограничение на количество наборов параметров в одном запросе (`--input-batch-max-rows`) или на задержку обработки (`--input-batch-max-delay`). Все полученные к этому моменту наборы параметров заворачиваются в `List<...>`, имя параметра задается опцией `--input-param-name`.</li></ul> |

В адаптивном режиме пакетирования доступны дополнительные параметры:

| Имя | Описание |
| ---|--- |
| `--input-batch-max-rows` | Максимальное количество наборов параметров в пакете для адаптивного режима пакетирования. Следующий пакет будет отправлен на исполнение вместе с запросом, если количество наборов данных в нем достигло указанного значения. Установка в `0` снимает ограничение.<br/><br/>Значение по умолчанию — `1000`.<br/><br/>Параметры передаются в запрос без стриминга и общий объем одного GRPC-запроса, в который включаются значения параметров, имеет верхнюю границу около 5 МБ. |
| `--input-batch-max-delay` | Максимальная задержка отправки на обработку полученного набора параметров для адаптивного режима пакетирования. Задается в виде числа с размерностью времени - `s` (секунды), `ms` (миллисекунды), `m` (минуты) и т.д. Значение по умолчанию — `1s` (1 секунда).<br/><br/>{{ ydb-short-name }} CLI будет отсчитывать время с момента получения первого набора параметров для пакета и отправит накопившийся пакет на исполнение, как только время превысит указанное значение. Параметр позволяет получить эффективное пакетирование в случае непредсказуемого темпа появления новых наборов параметров на `stdin`. |

### Примеры - полная пакетная обработка {#example-batch-full}

```bash
echo -e '{"a":10,"b":20}\n{"a":15,"b":25}\n{"a":35,"b":48}' | \
{{ ydb-cli }} -p quickstart sql \
  -s 'DECLARE $x AS List<Struct<a:Int64,b:Int64>>;
      SELECT ListLength($x), $x' \
  --input-framing newline-delimited \
  --input-param-name x \
  --input-batch full
```

Вывод команды:

```text
┌─────────┬───────────────────────────────────────────────────┐
| column0 | column1                                           |
├─────────┼───────────────────────────────────────────────────┤
| 3       | [{"a":10,"b":20},{"a":15,"b":25},{"a":35,"b":48}] |
└─────────┴───────────────────────────────────────────────────┘
```

### Примеры - адаптивная пакетная обработка {#example-batch-adaptive}

#### Ограничение максимальной задержки обработки {#example-adaptive-delay}

Для демонстрации работы адаптивного пакетирования со срабатыванием ограничения по задержке обработки в первой строке команды ниже производится генерация 1000 строк с задержкой в 0.2 секунды в `stdout`, которые передаются на `stdin` команде исполнения запроса. Команда исполнения запроса, в свою очередь, отображает пакеты параметров в каждом следующем вызове запроса.

```bash
for i in $(seq 1 1000); do echo "Line$i"; sleep 0.2; done | \
{{ ydb-cli }} -p quickstart sql \
  -s 'DECLARE $x AS List<Utf8>;
      SELECT ListLength($x), $x' \
  --input-framing newline-delimited \
  --input-format raw \
  --input-param-name x \
  --input-batch adaptive
```

Вывод команды (точные значения могут отличаться):

```text
┌─────────┬────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────┐
| column0 | column1                                                                                                                |
├─────────┼────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────┤
| 14      | ["Line1","Line2","Line3","Line4","Line5","Line6","Line7","Line8","Line9","Line10","Line11","Line12","Line13","Line14"] |
└─────────┴────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────┘
┌─────────┬─────────────────────────────────────────────────────────┐
| column0 | column1                                                 |
├─────────┼─────────────────────────────────────────────────────────┤
| 6       | ["Line15","Line16","Line17","Line18","Line19","Line20"] |
└─────────┴─────────────────────────────────────────────────────────┘
┌─────────┬─────────────────────────────────────────────────────────┐
| column0 | column1                                                 |
├─────────┼─────────────────────────────────────────────────────────┤
| 6       | ["Line21","Line22","Line23","Line24","Line25","Line26"] |
└─────────┴─────────────────────────────────────────────────────────┘
^C
```

В первый пакет попадают все строки, накопившиеся на входе за время открытия соединения с БД, и поэтому он больше чем последующие.

Выполнение команды можно прервать по Ctrl+C, или дождаться пока пройдут 200 секунд, в течение которых генерируется вход.

#### Ограничение по количеству записей {#example-adaptive-limit}

Для демонстрации работы адаптивного пакетирования со срабатыванием триггера по количеству наборов параметров в первой строке команды ниже производится генерация 200 строк. Команда будет отображать пакеты параметров в каждом следующем вызове запроса, учитывая заданное ограничение `--input-batch-max-rows` равное 20 (по умолчанию 1000).

В данном примере также показана возможность объединения параметров из разных источников, и формирование JSON на выходе.

```bash
for i in $(seq 1 200); do echo "Line$i"; done | \
{{ ydb-cli }} -p quickstart sql \
  -s 'DECLARE $x AS List<Utf8>;
      DECLARE $p2 AS Int64;
      SELECT ListLength($x) AS count, $p2 AS p2, $x AS items' \
  --input-framing newline-delimited \
  --input-format raw \
  --input-param-name x \
  --input-batch adaptive \
  --input-batch-max-rows 20 \
  --param '$p2=10' \
  --format json-unicode
```

Вывод команды:

```text
{"count":20,"p2":10,"items":["Line1","Line2","Line3","Line4","Line5","Line6","Line7","Line8","Line9","Line10","Line11","Line12","Line13","Line14","Line15","Line16","Line17","Line18","Line19","Line20"]}
{"count":20,"p2":10,"items":["Line21","Line22","Line23","Line24","Line25","Line26","Line27","Line28","Line29","Line30","Line31","Line32","Line33","Line34","Line35","Line36","Line37","Line38","Line39","Line40"]}
...
{"count":20,"p2":10,"items":["Line161","Line162","Line163","Line164","Line165","Line166","Line167","Line168","Line169","Line170","Line171","Line172","Line173","Line174","Line175","Line176","Line177","Line178","Line179","Line180"]}
{"count":20,"p2":10,"items":["Line181","Line182","Line183","Line184","Line185","Line186","Line187","Line188","Line189","Line190","Line191","Line192","Line193","Line194","Line195","Line196","Line197","Line198","Line199","Line200"]}
```

#### Удаление множества записей из строковой таблицы {{ ydb-short-name }} по первичным ключам {#example-adaptive-delete-pk}

{% include [not_allow_for_olap_note](../../_includes/not_allow_for_olap_note.md) %}

Если вы попытаетесь удалить большое количество строк из крупной таблицы с помощью простого запроса `DELETE FROM large_table WHERE id > 10;`, вы можете столкнуться с ошибкой из-за превышения ограничения на количество записей в транзакции. Данный пример показывает, как можно удалять неограниченное количество записей из таблиц {{ ydb-short-name }} без нарушения этого ограничения.
Создадим тестовую строковую таблицу:

```bash
{{ ydb-cli }} -p quickstart sql -s 'CREATE TABLE test_delete_1(id UInt64 NOT NULL, PRIMARY KEY (id))'
```

Занесем в неё 100,000 записей:

```bash
for i in $(seq 1 100000); do echo "$i";done | \
{{ ydb-cli }} -p quickstart import file csv -p test_delete_1
```

Удалим все записи со значениями `id` больше 10:

```bash
{{ ydb-cli }} -p quickstart sql \
  -s 'SELECT t.id FROM test_delete_1 AS t WHERE t.id > 10' \
  --format json-unicode | \
{{ ydb-cli }} -p quickstart sql \
  -s 'DECLARE $lines AS List<Struct<id:UInt64>>;
      DELETE FROM test_delete_1 WHERE id IN (SELECT tl.id FROM AS_TABLE($lines) AS tl)' \
  --input-framing newline-delimited \
  --input-param-name lines \
  --input-batch adaptive \
  --input-batch-max-rows 10000
```

#### Обработка сообщений, считываемых из топика {#example-adaptive-pipeline-from-topic}

Примеры обработки сообщений, считываемых из топика, приведены в статье [{#T}](topic-pipeline.md#example-read-to-yql-param).

## См. также {#see-also}

* [Параметризованные запросы в {{ ydb-short-name }} SDK](../ydb-sdk/parameterized_queries.md)
