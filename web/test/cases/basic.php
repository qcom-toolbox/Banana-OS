<!DOCTYPE html>
<html><body>
<h1><?= "Hello from PHP " . phpversion() ?></h1>
<?php
$name = "Banana";
$n = 7;
echo "<p>Name: $name, n squared: " . ($n * $n) . "</p>\n";
$fruits = ["apple", "banana", "cherry"];
$fruits[] = "date";
echo "<ul>\n";
foreach ($fruits as $i => $f) {
    echo "  <li>$i: " . ucfirst($f) . "</li>\n";
}
echo "</ul>\n";
$ages = array("Ann" => 31, "Bob" => 25, "Cy" => 40);
asort($ages);
foreach ($ages as $who => $age): ?>
  <p><?= $who ?> is <?= $age ?></p>
<?php endforeach;
function greet($who, $greeting = "Hi") {
    return "$greeting, {$who}!";
}
echo greet("World") . " " . greet("PHP", "Hello") . "\n";
if (count($fruits) > 3) {
    echo "many fruits\n";
} elseif (count($fruits) == 3) {
    echo "three\n";
} else {
    echo "few\n";
}
$total = 0;
for ($i = 1; $i <= 10; $i++) { $total += $i; }
echo "sum=$total\n";
$i = 0;
while ($i < 3) { echo $i; $i++; }
echo "\n";
echo strtoupper("abc"), " ", strlen("hello"), " ", str_repeat("=", 5), " ", substr("abcdef", 1, 3), " ", strrev("abc"), "\n";
echo implode(", ", array_map(fn($x) => $x * 2, [1, 2, 3])), "\n";
echo json_encode(["a" => 1, "b" => [1, 2], "c" => null]), " ", json_encode([1, 2, 3]), "\n";
echo sprintf("%05.2f|%-6s|%x|%d%%", 3.14159, "ab", 255, 42), "\n";
echo number_format(1234567.891, 2), " ", round(2.567, 1), " ", intval("12abc"), " ", max(3, 7, 1), "\n";
echo htmlspecialchars("<b>\"x\" & y</b>"), "\n";
$s = "x"; $s .= "y"; echo $s, " ", 10 / 4, " ", 7 % 3, " ", "5" + "5", " ", "5" . "5", "\n";
echo isset($undefined) ? "set" : "unset", " ", empty($fruits) ? "empty" : "not empty", " ", isset($fruits[1]) ? "has1" : "", "\n";
$counter = 0;
function bump() { global $counter; $counter++; }
bump(); bump();
echo "counter=$counter\n";
echo date("Y") >= 2024 ? "year ok" : "year bad", "\n";
var_dump(1.5, "hi", true, [1, "a" => 2]);
print_r(["x" => [1, 2]]);
echo "\n";
switch ($n) { case 7: echo "seven"; break; default: echo "other"; }
echo "\n";
try { throw new Exception("oops"); } catch (Exception $e) { echo "caught\n"; }
$m = [[1, 2], [3, 4]];
echo $m[1][0], " ", count($m), "\n";
?>
<p>The end.</p>
</body></html>
