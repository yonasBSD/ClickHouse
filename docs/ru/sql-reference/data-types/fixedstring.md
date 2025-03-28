---
slug: /ru/sql-reference/data-types/fixedstring
sidebar_position: 45
sidebar_label: FixedString(N)
---

# FixedString {#fixedstring}

Строка фиксированной длины `N` байт (не символов, не кодовых точек).

Чтобы объявить столбец типа `FixedString`, используйте следующий синтаксис:

``` sql
<column_name> FixedString(N)
```

Где `N` — натуральное число.

Тип `FixedString` эффективен, когда данные имеют длину ровно `N` байт. Во всех остальных случаях использование FixedString может привести к снижению эффективности.

Примеры значений, которые можно эффективно хранить в столбцах типа `FixedString`:

-   Двоичное представление IP-адреса (`FixedString(16)` для IPv6).
-   Коды языков (ru_RU, en_US ... ).
-   Коды валют (USD, RUB ... ).
-   Двоичное представление хэшей (`FixedString(16)` для MD5, `FixedString(32)` для SHA256).

Для хранения значений UUID используйте тип данных [UUID](uuid.md).

При вставке данных, ClickHouse:

-   Дополняет строку нулевыми байтами, если строка содержит меньше байтов, чем `N`.
-   Генерирует исключение `Too large value for FixedString(N)`, если строка содержит более `N` байт.

При выборе данных ClickHouse не обрезает нулевые байты в конце строки. Если вы используете секцию `WHERE`, то необходимо добавлять нулевые байты вручную, чтобы ClickHouse смог сопоставить выражение из фильтра значению `FixedString`. Следующий пример показывает, как использовать секцию `WHERE` с `FixedString`.

Рассмотрим следующую таблицу с единственным столбцом типа `FixedString(2)`:

``` text
┌─name──┐
│ b     │
└───────┘
```

Запрос `SELECT * FROM FixedStringTable WHERE a = 'b'` не возвращает необходимых данных. Необходимо дополнить шаблон фильтра нулевыми байтами.

``` sql
SELECT * FROM FixedStringTable
WHERE a = 'b\0'
```

``` text
┌─a─┐
│ b │
└───┘
```

Это поведение отличается от поведения MySQL для типа `CHAR`, где строки дополняются пробелами, а пробелы перед выводом вырезаются.

Обратите внимание, что длина значения `FixedString(N)` постоянна. Функция [length](/sql-reference/functions/array-functions#length) возвращает `N` даже если значение `FixedString(N)` заполнено только нулевыми байтами, однако функция [empty](../../sql-reference/data-types/fixedstring.md#empty) в этом же случае возвращает `1`.
