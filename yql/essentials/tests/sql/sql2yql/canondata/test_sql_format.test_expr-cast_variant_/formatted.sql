/* postgres can not */
/* syntax version 1 */
$a = AsEnum('a');
$b = AsEnum('b');
$c = AsEnum('c');
$d = AsEnum('d');
$t = TypeOf([Just($a), Just($b), just($c)]);

SELECT
    CAST([just($a), just($b), just($d)] AS $t)
;

$t0 = TypeOf([Just($a), Just($b)]);

SELECT
    CAST([$c, $d] AS $t0)
;
