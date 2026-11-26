/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "emojipick.h"
#include "i18n.h"
#include "theme.h"
#include "font.h"
#include "gfx.h"
#include "kbnav.h"
#include "keyboard.h"
#include "kstring.h"
#include "fs.h"

/* ── the emoji, by group, with the words a search finds them by ── */
enum { G_SMILEYS, G_PEOPLE, G_NATURE, G_FOOD, G_TRAVEL, G_ACTIVITIES, G_OBJECTS, G_SYMBOLS, NGROUPS };

typedef struct { const char* e; const char* name; uint8_t g; } emoji_t;

static const emoji_t EMOJI[] = {
    /* smileys & emotion */
    { "😀", "grinning face smile happy", G_SMILEYS }, { "😃", "grinning face big eyes smile", G_SMILEYS },
    { "😄", "grinning face smiling eyes", G_SMILEYS }, { "😁", "beaming face grin", G_SMILEYS },
    { "😆", "grinning squinting laugh", G_SMILEYS }, { "😅", "grinning sweat", G_SMILEYS },
    { "🤣", "rolling on the floor laughing rofl", G_SMILEYS }, { "😂", "tears of joy laugh lol", G_SMILEYS },
    { "🙂", "slightly smiling face", G_SMILEYS }, { "🙃", "upside down face", G_SMILEYS },
    { "😉", "winking face wink", G_SMILEYS }, { "😊", "smiling face blush", G_SMILEYS },
    { "😇", "halo angel innocent", G_SMILEYS }, { "🥰", "smiling hearts love", G_SMILEYS },
    { "😍", "heart eyes love", G_SMILEYS }, { "🤩", "star struck eyes", G_SMILEYS },
    { "😘", "blowing a kiss", G_SMILEYS }, { "😗", "kissing face", G_SMILEYS },
    { "😋", "savoring food yum tongue", G_SMILEYS }, { "😛", "tongue out", G_SMILEYS },
    { "😜", "winking tongue crazy", G_SMILEYS }, { "🤪", "zany face crazy", G_SMILEYS },
    { "😝", "squinting tongue", G_SMILEYS }, { "🤑", "money mouth face", G_SMILEYS },
    { "🤗", "hugging face hug", G_SMILEYS }, { "🤭", "hand over mouth oops", G_SMILEYS },
    { "🤫", "shushing face quiet", G_SMILEYS }, { "🤔", "thinking face hmm", G_SMILEYS },
    { "🤐", "zipper mouth", G_SMILEYS }, { "🤨", "raised eyebrow", G_SMILEYS },
    { "😐", "neutral face", G_SMILEYS }, { "😑", "expressionless", G_SMILEYS },
    { "😶", "no mouth", G_SMILEYS }, { "😏", "smirking face smirk", G_SMILEYS },
    { "😒", "unamused face", G_SMILEYS }, { "🙄", "rolling eyes", G_SMILEYS },
    { "😬", "grimacing face", G_SMILEYS }, { "😌", "relieved face", G_SMILEYS },
    { "😔", "pensive face", G_SMILEYS }, { "😪", "sleepy face", G_SMILEYS },
    { "🤤", "drooling face", G_SMILEYS }, { "😴", "sleeping face zzz", G_SMILEYS },
    { "😷", "face with medical mask sick", G_SMILEYS }, { "🤒", "thermometer ill fever", G_SMILEYS },
    { "🤕", "head bandage hurt", G_SMILEYS }, { "🤢", "nauseated face", G_SMILEYS },
    { "🤮", "vomiting face", G_SMILEYS }, { "🤧", "sneezing face", G_SMILEYS },
    { "🥵", "hot face", G_SMILEYS }, { "🥶", "cold face freezing", G_SMILEYS },
    { "😵", "dizzy face", G_SMILEYS }, { "🤯", "exploding head mind blown", G_SMILEYS },
    { "🤠", "cowboy hat face", G_SMILEYS }, { "🥳", "partying face party", G_SMILEYS },
    { "😎", "sunglasses cool", G_SMILEYS }, { "🤓", "nerd face glasses", G_SMILEYS },
    { "🧐", "monocle face", G_SMILEYS }, { "😕", "confused face", G_SMILEYS },
    { "😟", "worried face", G_SMILEYS }, { "🙁", "slightly frowning face sad", G_SMILEYS },
    { "😮", "open mouth surprised", G_SMILEYS }, { "😲", "astonished face", G_SMILEYS },
    { "😳", "flushed face", G_SMILEYS }, { "🥺", "pleading face puppy eyes", G_SMILEYS },
    { "😦", "frowning open mouth", G_SMILEYS }, { "😨", "fearful face scared", G_SMILEYS },
    { "😰", "anxious sweat", G_SMILEYS }, { "😢", "crying face tear sad", G_SMILEYS },
    { "😭", "loudly crying sob", G_SMILEYS }, { "😱", "screaming in fear", G_SMILEYS },
    { "😖", "confounded face", G_SMILEYS }, { "😞", "disappointed face", G_SMILEYS },
    { "😓", "downcast sweat", G_SMILEYS }, { "😩", "weary face", G_SMILEYS },
    { "😫", "tired face", G_SMILEYS }, { "🥱", "yawning face", G_SMILEYS },
    { "😤", "steam from nose triumph", G_SMILEYS }, { "😡", "pouting face angry rage", G_SMILEYS },
    { "😠", "angry face", G_SMILEYS }, { "🤬", "symbols on mouth cursing", G_SMILEYS },
    { "😈", "smiling horns devil", G_SMILEYS }, { "💀", "skull dead", G_SMILEYS },
    { "💩", "pile of poo", G_SMILEYS }, { "🤡", "clown face", G_SMILEYS },
    { "👻", "ghost", G_SMILEYS }, { "👽", "alien", G_SMILEYS },
    { "🤖", "robot", G_SMILEYS }, { "😺", "grinning cat", G_SMILEYS },
    { "🙈", "see no evil monkey", G_SMILEYS }, { "💋", "kiss mark lips", G_SMILEYS },
    { "💌", "love letter", G_SMILEYS }, { "💘", "heart with arrow", G_SMILEYS },
    { "💖", "sparkling heart", G_SMILEYS }, { "💔", "broken heart", G_SMILEYS },
    { "💛", "yellow heart", G_SMILEYS }, { "💚", "green heart", G_SMILEYS },
    { "💙", "blue heart", G_SMILEYS }, { "💜", "purple heart", G_SMILEYS },
    { "🖤", "black heart", G_SMILEYS }, { "💯", "hundred points 100", G_SMILEYS },
    { "💢", "anger symbol", G_SMILEYS }, { "💥", "collision boom", G_SMILEYS },
    { "💫", "dizzy star", G_SMILEYS }, { "💦", "sweat droplets", G_SMILEYS },
    { "💤", "zzz sleep", G_SMILEYS }, { "💬", "speech balloon", G_SMILEYS },
    /* people & body */
    { "👋", "waving hand hello bye", G_PEOPLE }, { "🤚", "raised back of hand", G_PEOPLE },
    { "🖐", "hand fingers splayed", G_PEOPLE }, { "👌", "ok hand", G_PEOPLE },
    { "🤞", "crossed fingers luck", G_PEOPLE }, { "🤟", "love you gesture", G_PEOPLE },
    { "🤘", "sign of the horns rock", G_PEOPLE }, { "🤙", "call me hand", G_PEOPLE },
    { "👈", "backhand index pointing left", G_PEOPLE }, { "👉", "backhand index pointing right", G_PEOPLE },
    { "👆", "index pointing up", G_PEOPLE }, { "👇", "index pointing down", G_PEOPLE },
    { "👍", "thumbs up like yes", G_PEOPLE }, { "👎", "thumbs down dislike no", G_PEOPLE },
    { "👊", "oncoming fist punch", G_PEOPLE }, { "👏", "clapping hands applause", G_PEOPLE },
    { "🙌", "raising hands hooray", G_PEOPLE }, { "👐", "open hands", G_PEOPLE },
    { "🤝", "handshake deal", G_PEOPLE }, { "🙏", "folded hands please thanks pray", G_PEOPLE },
    { "💪", "flexed biceps strong", G_PEOPLE }, { "🧠", "brain", G_PEOPLE },
    { "👀", "eyes look", G_PEOPLE }, { "👁", "eye", G_PEOPLE },
    { "👅", "tongue", G_PEOPLE }, { "👄", "mouth", G_PEOPLE },
    { "👶", "baby", G_PEOPLE }, { "🧒", "child", G_PEOPLE },
    { "👦", "boy", G_PEOPLE }, { "👧", "girl", G_PEOPLE },
    { "🧑", "person adult", G_PEOPLE }, { "👨", "man", G_PEOPLE },
    { "👩", "woman", G_PEOPLE }, { "🧓", "older person", G_PEOPLE },
    { "👴", "old man", G_PEOPLE }, { "👵", "old woman", G_PEOPLE },
    { "👮", "police officer", G_PEOPLE }, { "👷", "construction worker", G_PEOPLE },
    { "🤴", "prince", G_PEOPLE }, { "👸", "princess", G_PEOPLE },
    { "🎅", "santa claus christmas", G_PEOPLE }, { "🦸", "superhero", G_PEOPLE },
    { "🧙", "mage wizard", G_PEOPLE }, { "🧛", "vampire", G_PEOPLE },
    { "🧜", "merperson mermaid", G_PEOPLE }, { "🧚", "fairy", G_PEOPLE },
    { "🚶", "person walking", G_PEOPLE }, { "🏃", "person running", G_PEOPLE },
    { "💃", "woman dancing dance", G_PEOPLE }, { "🕺", "man dancing dance", G_PEOPLE },
    { "👪", "family", G_PEOPLE }, { "👫", "woman and man holding hands couple", G_PEOPLE },
    /* animals & nature */
    { "🐶", "dog face puppy", G_NATURE }, { "🐱", "cat face kitten", G_NATURE },
    { "🐭", "mouse face", G_NATURE }, { "🐹", "hamster", G_NATURE },
    { "🐰", "rabbit face bunny", G_NATURE }, { "🦊", "fox", G_NATURE },
    { "🐻", "bear", G_NATURE }, { "🐼", "panda", G_NATURE },
    { "🐨", "koala", G_NATURE }, { "🐯", "tiger face", G_NATURE },
    { "🦁", "lion", G_NATURE }, { "🐮", "cow face", G_NATURE },
    { "🐷", "pig face", G_NATURE }, { "🐸", "frog", G_NATURE },
    { "🐵", "monkey face", G_NATURE }, { "🐔", "chicken", G_NATURE },
    { "🐧", "penguin linux tux", G_NATURE }, { "🐦", "bird", G_NATURE },
    { "🐤", "baby chick", G_NATURE }, { "🦆", "duck", G_NATURE },
    { "🦅", "eagle", G_NATURE }, { "🦉", "owl", G_NATURE },
    { "🐺", "wolf", G_NATURE }, { "🐴", "horse face", G_NATURE },
    { "🦄", "unicorn", G_NATURE }, { "🐝", "honeybee bee", G_NATURE },
    { "🐛", "bug caterpillar", G_NATURE }, { "🦋", "butterfly", G_NATURE },
    { "🐌", "snail", G_NATURE }, { "🐞", "lady beetle ladybug", G_NATURE },
    { "🐢", "turtle", G_NATURE }, { "🐍", "snake", G_NATURE },
    { "🦖", "t-rex dinosaur", G_NATURE }, { "🐙", "octopus", G_NATURE },
    { "🦀", "crab", G_NATURE }, { "🐠", "tropical fish", G_NATURE },
    { "🐬", "dolphin", G_NATURE }, { "🐳", "spouting whale", G_NATURE },
    { "🦈", "shark", G_NATURE }, { "🐘", "elephant", G_NATURE },
    { "🦒", "giraffe", G_NATURE }, { "🐪", "camel", G_NATURE },
    { "🐾", "paw prints", G_NATURE }, { "💐", "bouquet flowers", G_NATURE },
    { "🌸", "cherry blossom flower", G_NATURE }, { "🌹", "rose flower", G_NATURE },
    { "🌻", "sunflower", G_NATURE }, { "🌷", "tulip", G_NATURE },
    { "🌱", "seedling plant", G_NATURE }, { "🌲", "evergreen tree", G_NATURE },
    { "🌳", "deciduous tree", G_NATURE }, { "🌴", "palm tree", G_NATURE },
    { "🌵", "cactus", G_NATURE }, { "🍀", "four leaf clover luck", G_NATURE },
    { "🍁", "maple leaf", G_NATURE }, { "🍄", "mushroom", G_NATURE },
    { "🌍", "globe europe africa earth world", G_NATURE }, { "🌎", "globe americas earth world", G_NATURE },
    { "🌙", "crescent moon night", G_NATURE }, { "🌞", "sun with face", G_NATURE },
    { "🌟", "glowing star", G_NATURE }, { "🌈", "rainbow", G_NATURE },
    { "🌊", "water wave sea", G_NATURE }, { "🔥", "fire hot", G_NATURE },
    { "💧", "droplet water", G_NATURE }, { "🌀", "cyclone", G_NATURE },
    /* food & drink */
    { "🍌", "banana", G_FOOD }, { "🍎", "red apple", G_FOOD },
    { "🍏", "green apple", G_FOOD }, { "🍐", "pear", G_FOOD },
    { "🍊", "tangerine orange", G_FOOD }, { "🍋", "lemon", G_FOOD },
    { "🍉", "watermelon", G_FOOD }, { "🍇", "grapes", G_FOOD },
    { "🍓", "strawberry", G_FOOD }, { "🍒", "cherries", G_FOOD },
    { "🍑", "peach", G_FOOD }, { "🥭", "mango", G_FOOD },
    { "🍍", "pineapple", G_FOOD }, { "🥥", "coconut", G_FOOD },
    { "🥝", "kiwi fruit", G_FOOD }, { "🍅", "tomato", G_FOOD },
    { "🥑", "avocado", G_FOOD }, { "🥕", "carrot", G_FOOD },
    { "🌽", "ear of corn", G_FOOD }, { "🥦", "broccoli", G_FOOD },
    { "🥔", "potato", G_FOOD }, { "🍞", "bread", G_FOOD },
    { "🥐", "croissant", G_FOOD }, { "🥖", "baguette bread", G_FOOD },
    { "🧀", "cheese", G_FOOD }, { "🥚", "egg", G_FOOD },
    { "🍳", "cooking fried egg", G_FOOD }, { "🥞", "pancakes", G_FOOD },
    { "🥓", "bacon", G_FOOD }, { "🍔", "hamburger burger", G_FOOD },
    { "🍟", "french fries", G_FOOD }, { "🍕", "pizza", G_FOOD },
    { "🌭", "hot dog", G_FOOD }, { "🌮", "taco", G_FOOD },
    { "🌯", "burrito", G_FOOD }, { "🍝", "spaghetti pasta", G_FOOD },
    { "🍜", "steaming bowl ramen noodles", G_FOOD }, { "🍣", "sushi", G_FOOD },
    { "🍚", "cooked rice", G_FOOD }, { "🍦", "soft ice cream", G_FOOD },
    { "🍩", "doughnut donut", G_FOOD }, { "🍪", "cookie", G_FOOD },
    { "🎂", "birthday cake", G_FOOD }, { "🍰", "shortcake cake", G_FOOD },
    { "🍫", "chocolate bar", G_FOOD }, { "🍬", "candy", G_FOOD },
    { "🍯", "honey pot", G_FOOD }, { "🍿", "popcorn", G_FOOD },
    { "☕", "hot beverage coffee tea", G_FOOD }, { "🍵", "teacup tea", G_FOOD },
    { "🥛", "glass of milk", G_FOOD }, { "🍺", "beer mug", G_FOOD },
    { "🍷", "wine glass", G_FOOD }, { "🍸", "cocktail glass", G_FOOD },
    { "🥂", "clinking glasses cheers", G_FOOD }, { "🧃", "beverage box juice", G_FOOD },
    /* travel & places */
    { "🚗", "automobile car", G_TRAVEL }, { "🚕", "taxi", G_TRAVEL },
    { "🚌", "bus", G_TRAVEL }, { "🚓", "police car", G_TRAVEL },
    { "🚑", "ambulance", G_TRAVEL }, { "🚒", "fire engine", G_TRAVEL },
    { "🚚", "delivery truck", G_TRAVEL }, { "🚜", "tractor", G_TRAVEL },
    { "🚲", "bicycle bike", G_TRAVEL }, { "🛴", "kick scooter", G_TRAVEL },
    { "🏍", "motorcycle", G_TRAVEL }, { "🚂", "locomotive train", G_TRAVEL },
    { "🚆", "train", G_TRAVEL }, { "🚇", "metro subway", G_TRAVEL },
    { "✈", "airplane plane", G_TRAVEL }, { "🚁", "helicopter", G_TRAVEL },
    { "🚀", "rocket launch", G_TRAVEL }, { "🛸", "flying saucer ufo", G_TRAVEL },
    { "⛵", "sailboat", G_TRAVEL }, { "🚢", "ship", G_TRAVEL },
    { "⚓", "anchor", G_TRAVEL }, { "⛽", "fuel pump gas", G_TRAVEL },
    { "🚦", "traffic light", G_TRAVEL }, { "🗺", "world map", G_TRAVEL },
    { "🏠", "house home", G_TRAVEL }, { "🏡", "house with garden", G_TRAVEL },
    { "🏢", "office building", G_TRAVEL }, { "🏥", "hospital", G_TRAVEL },
    { "🏫", "school", G_TRAVEL }, { "🏰", "castle", G_TRAVEL },
    { "🗼", "tokyo tower", G_TRAVEL }, { "🗽", "statue of liberty", G_TRAVEL },
    { "⛪", "church", G_TRAVEL }, { "⛺", "tent camping", G_TRAVEL },
    { "🏖", "beach with umbrella", G_TRAVEL }, { "🏝", "desert island", G_TRAVEL },
    { "🌋", "volcano", G_TRAVEL }, { "🗻", "mount fuji mountain", G_TRAVEL },
    { "🌃", "night with stars city", G_TRAVEL }, { "🌅", "sunrise", G_TRAVEL },
    { "🎡", "ferris wheel", G_TRAVEL }, { "🎢", "roller coaster", G_TRAVEL },
    { "⌛", "hourglass done", G_TRAVEL }, { "⏰", "alarm clock", G_TRAVEL },
    { "⌚", "watch", G_TRAVEL }, { "🌡", "thermometer", G_TRAVEL },
    { "⛅", "sun behind cloud weather", G_TRAVEL }, { "☔", "umbrella with rain drops", G_TRAVEL },
    { "⚡", "high voltage lightning", G_TRAVEL }, { "⛄", "snowman without snow", G_TRAVEL },
    /* activities */
    { "⚽", "soccer ball football", G_ACTIVITIES }, { "🏀", "basketball", G_ACTIVITIES },
    { "🏈", "american football", G_ACTIVITIES }, { "⚾", "baseball", G_ACTIVITIES },
    { "🎾", "tennis", G_ACTIVITIES }, { "🏐", "volleyball", G_ACTIVITIES },
    { "🏉", "rugby football", G_ACTIVITIES }, { "🎱", "pool 8 ball billiards", G_ACTIVITIES },
    { "🏓", "ping pong table tennis", G_ACTIVITIES }, { "🏸", "badminton", G_ACTIVITIES },
    { "🥊", "boxing glove", G_ACTIVITIES }, { "⛳", "flag in hole golf", G_ACTIVITIES },
    { "🎣", "fishing pole", G_ACTIVITIES }, { "🎿", "skis ski", G_ACTIVITIES },
    { "🏂", "snowboarder", G_ACTIVITIES }, { "🏊", "person swimming swim", G_ACTIVITIES },
    { "🚴", "person biking cycling", G_ACTIVITIES }, { "🏆", "trophy cup winner", G_ACTIVITIES },
    { "🥇", "1st place medal gold", G_ACTIVITIES }, { "🥈", "2nd place medal silver", G_ACTIVITIES },
    { "🥉", "3rd place medal bronze", G_ACTIVITIES }, { "🏅", "sports medal", G_ACTIVITIES },
    { "🎉", "party popper tada celebration", G_ACTIVITIES }, { "🎊", "confetti ball", G_ACTIVITIES },
    { "🎈", "balloon", G_ACTIVITIES }, { "🎁", "wrapped gift present", G_ACTIVITIES },
    { "🎄", "christmas tree", G_ACTIVITIES }, { "🎃", "jack-o-lantern halloween pumpkin", G_ACTIVITIES },
    { "🎆", "fireworks", G_ACTIVITIES }, { "✨", "sparkles", G_ACTIVITIES },
    { "🎮", "video game controller", G_ACTIVITIES }, { "🕹", "joystick", G_ACTIVITIES },
    { "🎲", "game die dice", G_ACTIVITIES }, { "🧩", "puzzle piece jigsaw", G_ACTIVITIES },
    { "♟", "chess pawn", G_ACTIVITIES }, { "🎯", "direct hit bullseye target", G_ACTIVITIES },
    { "🎨", "artist palette art paint", G_ACTIVITIES }, { "🎭", "performing arts theater", G_ACTIVITIES },
    { "🎬", "clapper board movie film", G_ACTIVITIES }, { "🎤", "microphone karaoke", G_ACTIVITIES },
    { "🎧", "headphone music", G_ACTIVITIES }, { "🎵", "musical note music", G_ACTIVITIES },
    { "🎶", "musical notes music", G_ACTIVITIES }, { "🎸", "guitar", G_ACTIVITIES },
    { "🎹", "musical keyboard piano", G_ACTIVITIES }, { "🎺", "trumpet", G_ACTIVITIES },
    { "🎻", "violin", G_ACTIVITIES }, { "🥁", "drum", G_ACTIVITIES },
    /* objects */
    { "💻", "laptop computer", G_OBJECTS }, { "🖥", "desktop computer", G_OBJECTS },
    { "⌨", "keyboard", G_OBJECTS }, { "🖱", "computer mouse", G_OBJECTS },
    { "💾", "floppy disk save", G_OBJECTS }, { "💿", "optical disk cd", G_OBJECTS },
    { "📀", "dvd", G_OBJECTS }, { "📱", "mobile phone", G_OBJECTS },
    { "☎", "telephone", G_OBJECTS }, { "📷", "camera photo", G_OBJECTS },
    { "📺", "television tv", G_OBJECTS }, { "📻", "radio", G_OBJECTS },
    { "🔋", "battery", G_OBJECTS }, { "🔌", "electric plug", G_OBJECTS },
    { "💡", "light bulb idea", G_OBJECTS }, { "🔦", "flashlight torch", G_OBJECTS },
    { "🕯", "candle", G_OBJECTS }, { "📖", "open book read", G_OBJECTS },
    { "📚", "books", G_OBJECTS }, { "📝", "memo note write", G_OBJECTS },
    { "✏", "pencil", G_OBJECTS }, { "📎", "paperclip", G_OBJECTS },
    { "📌", "pushpin pin", G_OBJECTS }, { "📁", "file folder", G_OBJECTS },
    { "📂", "open file folder", G_OBJECTS }, { "📅", "calendar date", G_OBJECTS },
    { "📈", "chart increasing", G_OBJECTS }, { "📉", "chart decreasing", G_OBJECTS },
    { "📊", "bar chart", G_OBJECTS }, { "📋", "clipboard", G_OBJECTS },
    { "✂", "scissors cut", G_OBJECTS }, { "🔒", "locked lock", G_OBJECTS },
    { "🔓", "unlocked", G_OBJECTS }, { "🔑", "key", G_OBJECTS },
    { "🔨", "hammer", G_OBJECTS }, { "🔧", "wrench", G_OBJECTS },
    { "⚙", "gear settings", G_OBJECTS }, { "🧰", "toolbox", G_OBJECTS },
    { "🔬", "microscope", G_OBJECTS }, { "🔭", "telescope", G_OBJECTS },
    { "💊", "pill medicine", G_OBJECTS }, { "🛒", "shopping cart", G_OBJECTS },
    { "💰", "money bag", G_OBJECTS }, { "💳", "credit card", G_OBJECTS },
    { "✉", "envelope mail", G_OBJECTS }, { "📧", "e-mail email", G_OBJECTS },
    { "📦", "package box", G_OBJECTS }, { "🔔", "bell notification", G_OBJECTS },
    { "📣", "megaphone", G_OBJECTS }, { "🔍", "magnifying glass search", G_OBJECTS },
    { "⏳", "hourglass", G_OBJECTS }, { "🎓", "graduation cap", G_OBJECTS },
    { "👑", "crown king queen", G_OBJECTS }, { "👓", "glasses", G_OBJECTS },
    { "👕", "t-shirt", G_OBJECTS }, { "👖", "jeans", G_OBJECTS },
    { "👟", "running shoe sneaker", G_OBJECTS }, { "🎒", "backpack", G_OBJECTS },
    { "💎", "gem stone diamond", G_OBJECTS }, { "🧸", "teddy bear", G_OBJECTS },
    /* symbols */
    { "❤", "red heart love", G_SYMBOLS }, { "🧡", "orange heart", G_SYMBOLS },
    { "💕", "two hearts", G_SYMBOLS }, { "💞", "revolving hearts", G_SYMBOLS },
    { "✅", "check mark button done yes", G_SYMBOLS }, { "✔", "check mark", G_SYMBOLS },
    { "❌", "cross mark no wrong", G_SYMBOLS }, { "❎", "cross mark button", G_SYMBOLS },
    { "➕", "plus", G_SYMBOLS }, { "➖", "minus", G_SYMBOLS },
    { "➗", "divide", G_SYMBOLS }, { "❓", "question mark", G_SYMBOLS },
    { "❗", "exclamation mark", G_SYMBOLS }, { "⚠", "warning", G_SYMBOLS },
    { "⛔", "no entry", G_SYMBOLS }, { "🚫", "prohibited forbidden", G_SYMBOLS },
    { "♻", "recycling symbol", G_SYMBOLS }, { "🔴", "red circle", G_SYMBOLS },
    { "🟠", "orange circle", G_SYMBOLS }, { "🟡", "yellow circle", G_SYMBOLS },
    { "🟢", "green circle", G_SYMBOLS }, { "🔵", "blue circle", G_SYMBOLS },
    { "🟣", "purple circle", G_SYMBOLS }, { "⚫", "black circle", G_SYMBOLS },
    { "⚪", "white circle", G_SYMBOLS }, { "🟥", "red square", G_SYMBOLS },
    { "🟩", "green square", G_SYMBOLS }, { "🟦", "blue square", G_SYMBOLS },
    { "⬛", "black large square", G_SYMBOLS }, { "⬜", "white large square", G_SYMBOLS },
    { "⭐", "star", G_SYMBOLS }, { "⭕", "hollow red circle", G_SYMBOLS },
    { "⬆", "up arrow", G_SYMBOLS }, { "⬇", "down arrow", G_SYMBOLS },
    { "⬅", "left arrow", G_SYMBOLS }, { "➡", "right arrow", G_SYMBOLS },
    { "🔄", "counterclockwise arrows refresh", G_SYMBOLS }, { "🔁", "repeat button", G_SYMBOLS },
    { "▶", "play button", G_SYMBOLS }, { "⏸", "pause button", G_SYMBOLS },
    { "⏹", "stop button", G_SYMBOLS }, { "⏩", "fast forward", G_SYMBOLS },
    { "🔇", "muted speaker", G_SYMBOLS }, { "🔊", "speaker high volume", G_SYMBOLS },
    { "📶", "antenna bars signal", G_SYMBOLS }, { "🆗", "ok button", G_SYMBOLS },
    { "🆕", "new button", G_SYMBOLS }, { "🆒", "cool button", G_SYMBOLS },
    { "🆘", "sos button", G_SYMBOLS }, { "♈", "aries zodiac", G_SYMBOLS },
    { "☮", "peace symbol", G_SYMBOLS }, { "☯", "yin yang", G_SYMBOLS },
    { "♾", "infinity", G_SYMBOLS }, { "©", "copyright", G_SYMBOLS },
    { "®", "registered", G_SYMBOLS }, { "™", "trade mark", G_SYMBOLS },
    { "🏁", "chequered flag finish", G_SYMBOLS }, { "🚩", "triangular flag", G_SYMBOLS },
    { "🏳", "white flag", G_SYMBOLS }, { "🏴", "black flag", G_SYMBOLS },
};
#define NEMOJI ((int)(sizeof(EMOJI) / sizeof(EMOJI[0])))

/* the tabs: the ones used lately, then the groups - each with its icon */
#define TAB_RECENT 0
#define NTABS (NGROUPS + 1)
static const char* const TAB_ICON[NTABS] = { "🕘", "😀", "👋", "🐶", "🍌", "🚗", "⚽", "💡", "❤" };
static const char* const TAB_NAME[NTABS] = {
    "Most recently used", "Smileys & emotion", "People & body", "Animals & nature", "Food & drink",
    "Travel & places", "Activities", "Objects", "Symbols",
};

/* ── state ── */
#define RECENT_MAX  24
#define RECENT_FILE "/etc/emoji-recent"
#define COLS        8
#define ROWS        6
#define CELL        40
#define HEAD_H      38
#define TABS_H      32
#define FOOT_H      24
#define PW          (COLS * CELL + 16)
#define PH          (HEAD_H + TABS_H + ROWS * CELL + 8 + FOOT_H)
#define C_BG        TC(0x1B212B)
#define C_LINE      TC(0x505E74)
#define C_CELL_SEL  TC(0x405478)
#define C_TEXT      TC(0xE8EEF6)
#define C_DIM       TC(0x9AA6B6)
#define C_ACCENT    TC(0xFFD34E)

static int  g_open, g_tab = 1, g_sel, g_top;
static char g_query[32];
static int  g_recent[RECENT_MAX], g_nrecent, g_loaded;
static int  g_items[sizeof(EMOJI) / sizeof(EMOJI[0])], g_nitems;

/* ── the emoji used lately: /etc/emoji-recent, one a line, the newest first ── */
static int find_emoji(const char* s, int n) {
    for (int i = 0; i < NEMOJI; i++)
        if ((int)strlen(EMOJI[i].e) == n && !memcmp(EMOJI[i].e, s, (size_t)n)) return i;
    return -1;
}

static void recent_load(void) {
    if (g_loaded) return;
    g_loaded = 1;
    int fi = fs_find_file(RECENT_FILE);
    fs_file_t* f = fi >= 0 ? fs_get_file(fi) : NULL;
    if (!f || !f->content) return;
    const char* p = f->content, *end = f->content + f->size;
    while (p < end && g_nrecent < RECENT_MAX) {
        const char* e = p;
        while (e < end && *e != '\n') e++;
        int i = find_emoji(p, (int)(e - p));
        if (i >= 0) g_recent[g_nrecent++] = i;
        p = e < end ? e + 1 : end;
    }
}

static void recent_use(int idx) {
    int n = 0, tmp[RECENT_MAX];
    tmp[n++] = idx;
    for (int i = 0; i < g_nrecent && n < RECENT_MAX; i++) if (g_recent[i] != idx) tmp[n++] = g_recent[i];
    memcpy(g_recent, tmp, sizeof(int) * (size_t)n);
    g_nrecent = n;
    char buf[RECENT_MAX * 6];
    uint32_t off = 0;
    for (int i = 0; i < g_nrecent; i++) {
        uint32_t l = (uint32_t)strlen(EMOJI[g_recent[i]].e);
        memcpy(buf + off, EMOJI[g_recent[i]].e, l);
        off += l;
        buf[off++] = '\n';
    }
    fs_mkdir_p_sys("/etc");
    fs_write_path_sys(RECENT_FILE, buf, off);
}

/* ── what the grid shows: a search's matches, else the tab's emoji ── */
static char lower(char c) { return c >= 'A' && c <= 'Z' ? (char)(c + 32) : c; }

/* every word of the query starts a word of the name */
static int matches(const char* name, const char* q) {
    while (*q) {
        while (*q == ' ') q++;
        if (!*q) break;
        int ql = 0;
        while (q[ql] && q[ql] != ' ') ql++;
        int found = 0;
        for (const char* w = name; *w && !found; ) {
            int k = 0;
            while (k < ql && w[k] && lower(w[k]) == lower(q[k])) k++;
            if (k == ql) found = 1;
            while (*w && *w != ' ') w++;
            while (*w == ' ') w++;
        }
        if (!found) return 0;
        q += ql;
    }
    return 1;
}

static void collect(void) {
    g_nitems = 0;
    if (g_query[0]) {
        for (int i = 0; i < NEMOJI; i++) if (matches(EMOJI[i].name, g_query)) g_items[g_nitems++] = i;
    } else if (g_tab == TAB_RECENT) {
        for (int i = 0; i < g_nrecent; i++) g_items[g_nitems++] = g_recent[i];
    } else {
        for (int i = 0; i < NEMOJI; i++) if (EMOJI[i].g == g_tab - 1) g_items[g_nitems++] = i;
    }
}

static void clamp(void) {
    collect();
    if (g_sel >= g_nitems) g_sel = g_nitems - 1;
    if (g_sel < 0) g_sel = 0;
    int row = g_sel / COLS, rows = (g_nitems + COLS - 1) / COLS;
    if (row < g_top) g_top = row;
    if (row >= g_top + ROWS) g_top = row - ROWS + 1;
    if (g_top > rows - ROWS) g_top = rows - ROWS;
    if (g_top < 0) g_top = 0;
}

int ep_search_count(const char* query, const char** first) {
    int n = 0;
    if (first) *first = NULL;
    for (int i = 0; i < NEMOJI; i++)
        if (matches(EMOJI[i].name, query)) { if (!n && first) *first = EMOJI[i].e; n++; }
    return n;
}

/* ── open / close ── */
int ep_is_open(void) { return g_open; }

void ep_close(void) { g_open = 0; }

void ep_toggle(void) {
    if (g_open) { g_open = 0; return; }
    recent_load();
    g_open = 1;
    g_query[0] = 0;
    g_tab = g_nrecent ? TAB_RECENT : 1;
    g_sel = g_top = 0;
}

/* the emoji typed into the window in front, as if from the keyboard */
static void insert(int item) {
    if (item < 0 || item >= g_nitems) return;
    int idx = g_items[item];
    g_open = 0;
    keyboard_inject(EMOJI[idx].e);
    recent_use(idx);
}

/* ── drawing ── */
static void geom(const fb_info_t* fi, int bar_h, int* px, int* py) {
    int W = (int)fi->width, H = (int)fi->height;
    *px = W - PW - 12;
    *py = H - bar_h - PH - 10;
    if (*px < 0) *px = 0;
    if (*py < 0) *py = 0;
}

static void frame(int x, int y, int w, int h, uint32_t bg, uint32_t line) {
    gfx_fill_rect(x, y, w, h, bg);
    gfx_fill_rect(x, y, w, 1, line);
    gfx_fill_rect(x, y + h - 1, w, 1, line);
    gfx_fill_rect(x, y, 1, h, line);
    gfx_fill_rect(x + w - 1, y, 1, h, line);
}

/* an emoji centred in the w x h box at (x, y), px pixels to the em */
static void draw_emoji(int x, int y, int w, int h, int px, const char* e, uint32_t color) {
    int stride, tw, th;
    uint32_t* t = fb_target(&stride, &tw, &th);
    if (!t) return;
    int face = font_available(FONT_EMOJI) ? FONT_EMOJI : FONT_SANS;
    uint32_t n = (uint32_t)strlen(e);
    int adv = font_text_width(face, px, e, n);
    int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
    int x1 = x + w > tw ? tw : x + w, y1 = y + h > th ? th : y + h;
    font_draw(t, stride, x0, y0, x1, y1, face, px, x + (w - adv) / 2, y + h / 2 + px * 3 / 8, e, n, color, 0);
}

void ep_draw(const fb_info_t* fi, int bar_h) {
    if (!g_open || !fi) return;
    clamp();
    int px, py;
    geom(fi, bar_h, &px, &py);
    frame(px, py, PW, PH, C_BG, C_LINE);

    /* the search box */
    int sx = px + 8, sy = py + 8, sw = PW - 16, sh = 22;
    frame(sx, sy, sw, sh, TC(0x262D38), g_query[0] ? TC(0x90B8F0) : TC(0x384252));
    if (g_query[0]) {
        gfx_draw_text(sx + 8, sy + 7, g_query, C_TEXT, TC(0x262D38));
        gfx_fill_rect(sx + 8 + (int)strlen(g_query) * 8, sy + 4, 1, 14, C_TEXT);
    } else {
        gfx_draw_text(sx + 8, sy + 7, _("Type to search emoji"), C_DIM, TC(0x262D38));
    }

    /* the tabs */
    int ty = py + HEAD_H, tabw = (PW - 16) / NTABS;
    for (int i = 0; i < NTABS; i++) {
        int tx = px + 8 + i * tabw, on = !g_query[0] && i == g_tab;
        draw_emoji(tx, ty, tabw, TABS_H - 4, 18, TAB_ICON[i], on ? TC(0xFFFFFF) : TC(0x8090A4));
        if (on) gfx_fill_rect(tx + 6, ty + TABS_H - 4, tabw - 12, 2, C_ACCENT);
    }
    gfx_fill_rect(px + 8, ty + TABS_H - 1, PW - 16, 1, TC(0x384252));

    /* the grid */
    int gx = px + 8, gy = py + HEAD_H + TABS_H + 4;
    for (int r = 0; r < ROWS; r++)
        for (int c = 0; c < COLS; c++) {
            int item = (g_top + r) * COLS + c;
            if (item >= g_nitems) break;
            int cx = gx + c * CELL, cy = gy + r * CELL;
            if (item == g_sel) frame(cx + 1, cy + 1, CELL - 2, CELL - 2, C_CELL_SEL, TC(0x90B8F0));
            draw_emoji(cx, cy, CELL, CELL, 26, EMOJI[g_items[item]].e, TC(0xF2F5FA));
        }
    if (!g_nitems)
        gfx_draw_text(gx + 8, gy + 16, g_query[0] ? "No emoji found." :
                      g_tab == TAB_RECENT ? "The emoji you use show up here." : "", C_DIM, C_BG);

    /* the footer: the chosen emoji's name, or the group's */
    int fy = py + PH - FOOT_H;
    gfx_fill_rect(px + 8, fy - 2, PW - 16, 1, TC(0x384252));
    char line[48];
    if (g_nitems) {
        const char* nm = EMOJI[g_items[g_sel]].name;
        int k = 0;
        while (nm[k] && k < (int)sizeof(line) - 1 && k < 26) { line[k] = nm[k]; k++; }
        line[k] = 0;
    } else {
        kstrlcpy(line, g_query[0] ? "Search" : TAB_NAME[g_tab], sizeof(line));
    }
    gfx_draw_text(px + 12, fy + 7, line, C_TEXT, C_BG);
    gfx_draw_text(px + PW - 12 - 13 * 8, fy + 7, _("Enter: insert"), C_DIM, C_BG);
}

/* ── keys and the mouse ── */
int ep_key(int code) {
    int qn = (int)strlen(g_query);
    if (code == KB_ESC) {
        if (qn) { g_query[0] = 0; g_sel = g_top = 0; }
        else g_open = 0;
        return 1;
    }
    collect();
    if (code == KB_LEFT) g_sel--;
    else if (code == KB_RIGHT) g_sel++;
    else if (code == KB_UP) { if (g_sel >= COLS) g_sel -= COLS; }
    else if (code == KB_DOWN) { if (g_sel + COLS < g_nitems) g_sel += COLS; else if (g_sel / COLS < (g_nitems - 1) / COLS) g_sel = g_nitems - 1; }
    else if (code == KB_PGUP) g_sel -= COLS * ROWS;
    else if (code == KB_PGDN) g_sel += COLS * ROWS;
    else if (code == KB_HOME) g_sel = 0;
    else if (code == KB_END) g_sel = g_nitems - 1;
    else if (code == KB_TAB || code == KB_BACKTAB) {
        g_query[0] = 0;
        g_tab = (g_tab + (code == KB_TAB ? 1 : NTABS - 1)) % NTABS;
        g_sel = g_top = 0;
    } else if (code == KB_ENTER || (code == KB_SPACE && !qn)) {
        insert(g_sel);
        return 1;
    } else if (code == '\b' || code == 127) {
        if (qn) { g_query[qn - 1] = 0; g_sel = g_top = 0; }
    } else if ((code == KB_SPACE || (code > ' ' && code < 127)) && qn < (int)sizeof(g_query) - 1) {
        g_query[qn] = code == KB_SPACE ? ' ' : (char)code;
        g_query[qn + 1] = 0;
        g_sel = g_top = 0;
    }
    clamp();
    return 1;
}

void ep_click(const fb_info_t* fi, int bar_h, int mx, int my) {
    int px, py;
    geom(fi, bar_h, &px, &py);
    if (mx < px || mx >= px + PW || my < py || my >= py + PH) { g_open = 0; return; }
    clamp();
    int ty = py + HEAD_H, tabw = (PW - 16) / NTABS;
    if (my >= ty && my < ty + TABS_H) {
        int i = (mx - px - 8) / tabw;
        if (mx >= px + 8 && i >= 0 && i < NTABS) { g_tab = i; g_query[0] = 0; g_sel = g_top = 0; }
        return;
    }
    int gx = px + 8, gy = py + HEAD_H + TABS_H + 4;
    if (mx < gx || my < gy) return;
    int c = (mx - gx) / CELL, r = (my - gy) / CELL;
    if (c >= COLS || r >= ROWS) return;
    int item = (g_top + r) * COLS + c;
    if (item < g_nitems) { g_sel = item; insert(item); }
}

int ep_wheel(const fb_info_t* fi, int bar_h, int mx, int my, int dz) {
    int px, py;
    geom(fi, bar_h, &px, &py);
    if (mx < px || mx >= px + PW || my < py || my >= py + PH) return 0;
    collect();
    int rows = (g_nitems + COLS - 1) / COLS;
    g_top += dz > 0 ? 1 : -1;
    if (g_top > rows - ROWS) g_top = rows - ROWS;
    if (g_top < 0) g_top = 0;
    if (g_sel / COLS < g_top) g_sel = g_top * COLS;
    if (g_sel / COLS >= g_top + ROWS) g_sel = (g_top + ROWS - 1) * COLS;
    return 1;
}
