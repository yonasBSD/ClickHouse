---
slug: /ru/sql-reference/table-functions/remote
sidebar_position: 40
sidebar_label: remote
---

# remote, remoteSecure {#remote-remotesecure}

Позволяет обратиться к удалённым серверам без создания таблицы типа [Distributed](../../engines/table-engines/special/distributed.md). Функция `remoteSecure` работает аналогично `remote`, но использует защищенное соединение.

Обе функции могут использоваться в запросах `SELECT` и `INSERT`.

**Синтаксис**

``` sql
remote('addresses_expr', db, table[, 'user'[, 'password']])
remote('addresses_expr', db.table[, 'user'[, 'password']])
remoteSecure('addresses_expr', db, table[, 'user'[, 'password']])
remoteSecure('addresses_expr', db.table[, 'user'[, 'password']])
```

**Параметры**

- `addresses_expr` — выражение, генерирующее адреса удалённых серверов. Это может быть просто один адрес сервера. Адрес сервера — это `host:port` или только `host`.

    Вместо параметра `host` может быть указано имя сервера или его адрес в формате IPv4 или IPv6. IPv6 адрес указывается в квадратных скобках.

	`port` — TCP-порт удалённого сервера. Если порт не указан, используется [tcp_port](../../operations/server-configuration-parameters/settings.md#server_configuration_parameters-tcp_port) из конфигурационного файла сервера, к которому обратились через функцию `remote` (по умолчанию - 9000), и [tcp_port_secure](../../operations/server-configuration-parameters/settings.md#server_configuration_parameters-tcp_port_secure), к которому обратились через функцию `remoteSecure` (по умолчанию — 9440).

    С IPv6-адресом обязательно нужно указывать порт.

    Тип: [String](../../sql-reference/data-types/string.md).

- `db` — имя базы данных. Тип: [String](../../sql-reference/data-types/string.md).
- `table` — имя таблицы. Тип: [String](../../sql-reference/data-types/string.md).
- `user` — имя пользователя. Если пользователь не указан, то по умолчанию `default`. Тип: [String](../../sql-reference/data-types/string.md).
- `password` — пароль. Если пароль не указан, то используется пустой пароль. Тип: [String](../../sql-reference/data-types/string.md).
- `sharding_key` — ключ шардирования для поддержки распределения данных между узлами. Например: `insert into remote('127.0.0.1:9000,127.0.0.2', db, table, 'default', rand())`. Тип: [UInt32](../../sql-reference/data-types/int-uint.md).

**Возвращаемое значение**

Набор данных с удаленных серверов.

**Использование**

Использование табличной функции `remote` менее оптимально, чем создание таблицы типа `Distributed`, так как в этом случае соединения с серверами устанавливаются заново при каждом запросе. Если указываются имена серверов, то приходится также выполнять поиск сервера по имени. Кроме того, не ведётся сквозной подсчёт ошибок при работе с разными репликами. При обработке большого количества запросов всегда создавайте таблицу типа `Distributed`, использовать табличную функцию `remote` в таких случаях не рекомендуется.

Табличная функция `remote` может быть полезна в следующих случаях:

-   Обращение на конкретный сервер для сравнения данных, отладки и тестирования.
-   Запросы между разными кластерами ClickHouse для исследований.
-   Нечастые распределённые запросы, задаваемые вручную.
-   Распределённые запросы, где набор серверов определяется каждый раз заново.

**Адреса**

``` text
example01-01-1
example01-01-1:9000
localhost
127.0.0.1
[::]:9000
[2a02:6b8:0:1111::11]:9000
```

Адреса можно указать через запятую. В этом случае ClickHouse обработает запрос как распределённый, т.е. отправит его по всем указанным адресам как на шарды с разными данными. Пример:

``` text
example01-01-1,example01-02-1
```

**Примеры**

Выборка данных с удаленного сервера:

``` sql
SELECT * FROM remote('127.0.0.1', db.remote_engine_table) LIMIT 3;
```

Вставка данных с удаленного сервера в таблицу:

``` sql
CREATE TABLE remote_table (name String, value UInt32) ENGINE=Memory;
INSERT INTO FUNCTION remote('127.0.0.1', currentDatabase(), 'remote_table') VALUES ('test', 42);
SELECT * FROM remote_table;
```

## Символы подстановки в адресах {#globs-in-addresses}

Шаблоны в фигурных скобках `{ }` используются, чтобы сгенерировать список шардов или указать альтернативный адрес на случай отказа. В одном URL можно использовать несколько шаблонов.
Поддерживаются следующие типы шаблонов.

- `{*a*,*b*}` - несколько вариантов, разделенных запятой. Весь шаблон заменяется на *a* в адресе первого шарда, заменяется на *b* в адресе второго шарда и так далее. Например, `example0{1,2}-1` генерирует адреса `example01-1` и `example02-1`.
- `{*n*..*m*}` - диапазон чисел. Этот шаблон генерирует адреса шардов с увеличивающимися индексами от *n* до *m*. `example0{1..2}-1` генерирует `example01-1` и `example02-1`.
- `{*0n*..*0m*}` - диапазон чисел с ведущими нулями. Такой вариант сохраняет ведущие нули в индексах. По шаблону `example{01..03}-1` генерируются `example01-1`, `example02-1` и `example03-1`.
- `{*a*|*b*}` - несколько вариантов, разделенных `|`. Шаблон задает адреса реплик. Например, `example01-{1|2}` генерирует реплики`example01-1` и `example01-2`.

Запрос будет отправлен на первую живую реплику. При этом для `remote` реплики перебираются в порядке, заданном настройкой [load_balancing](../../operations/settings/settings.md#settings-load_balancing).
Количество генерируемых адресов ограничено настройкой [table_function_remote_max_addresses](../../operations/settings/settings.md#table_function_remote_max_addresses).
