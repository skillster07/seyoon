// Converts the UTF-8 source into an ASCII-only JSX (non-ASCII -> \uXXXX) so ExtendScript reads Korean safely.
const fs = require('fs');
const src = fs.readFileSync(__dirname + '/BUILD_SC01_v03.src.jsx', 'utf8');
const out = src.replace(/[^\x00-\x7f]/g, c => '\\u' + c.charCodeAt(0).toString(16).padStart(4, '0'));
fs.writeFileSync(__dirname + '/../BUILD_SC01_v03.jsx', out);
