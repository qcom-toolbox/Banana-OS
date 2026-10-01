<?php
$name = isset($_GET['name']) ? trim($_GET['name']) : '';
$fruits = ['banana' => 105, 'apple' => 95, 'cherry' => 50, 'mango' => 200];
arsort($fruits);

function greeting($who) {
    $h = (int)date('G');
    $part = $h < 12 ? 'Good morning' : ($h < 18 ? 'Good afternoon' : 'Good evening');
    return $who === '' ? "$part!" : "$part, " . htmlspecialchars($who) . "!";
}

$book = 'guestbook.txt';
if (isset($_GET['msg']) && trim($_GET['msg']) !== '' && function_exists('file_put_contents')) {
    $line = date('H:i') . ' ' . str_replace("\n", ' ', trim($_GET['msg'])) . "\n";
    file_put_contents($book, $line, FILE_APPEND);
}
?>
<!DOCTYPE html>
<html>
<head>
<title>PHP on Banana OS</title>
<style>
  body { background: #f7f3e3; margin: 0; }
  .top { background: #6c5ce7; color: #fff; padding: 10px 18px; }
  .top h1 { margin: 0; font-size: 26px; }
  .box { background: #fff; border: 1px solid #d8cfa8; margin: 10px 18px; padding: 8px 12px; }
  h2 { font-size: 16px; color: #4b3fb0; margin: 2px 0 6px 0; }
  table { border-collapse: collapse; }
  td, th { border: 1px solid #ccc; padding: 2px 8px; }
  th { background: #efeafc; }
  .big { color: #c0392b; font-weight: bold; }
</style>
</head>
<body>
<div class="top"><h1>PHP <?= PHP_VERSION ?> on Banana OS</h1>
Served by <?= $_SERVER['SERVER_SOFTWARE'] ?> at <?= date('Y-m-d H:i:s') ?></div>

<div class="box">
  <h2><?= greeting($name) ?></h2>
  <form action="demo.php">
    Your name: <input name="name" value="<?= htmlspecialchars($name) ?>">
    <input type="submit" value="Say hello">
  </form>
</div>

<div class="box">
  <h2>Calories (sorted with arsort)</h2>
  <table>
    <tr><th>#</th><th>Fruit</th><th>kcal</th></tr>
<?php $i = 1; foreach ($fruits as $fruit => $kcal): ?>
    <tr><td><?= $i++ ?></td><td><?= ucfirst($fruit) ?></td>
        <td<?php if ($kcal > 100) echo ' class="big"'; ?>><?= $kcal ?></td></tr>
<?php endforeach; ?>
  </table>
  <p>Total: <?= array_sum($fruits) ?> kcal, average <?= number_format(array_sum($fruits) / count($fruits), 1) ?>.</p>
</div>

<div class="box">
  <h2>Loops and math</h2>
  <p>Squares: <?php for ($n = 1; $n <= 10; $n++) { echo $n * $n, $n < 10 ? ', ' : ''; } ?></p>
  <p>2<sup>16</sup> = <?= pow(2, 16) ?>, sqrt(2) = <?= round(sqrt(2), 6) ?>, md5("banana") = <?= md5("banana") ?></p>
</div>

<div class="box">
  <h2>Guestbook</h2>
  <form action="demo.php">
    <input name="msg" placeholder="Leave a message"> <input type="submit" value="Sign">
  </form>
  <ul>
<?php
$lines = function_exists('file') && file_exists($book) ? file($book) : [];
if (count($lines) == 0) echo "    <li><i>No messages yet.</i></li>\n";
foreach (array_reverse($lines) as $l) echo "    <li>" . htmlspecialchars(trim($l)) . "</li>\n";
?>
  </ul>
</div>

<div class="box">
  <h2>JavaScript in a PHP page</h2>
  <button onclick="this.textContent = 'Clicked at ' + new Date().toLocaleTimeString()">Click me</button>
</div>
</body>
</html>
