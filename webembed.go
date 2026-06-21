package flux

import "embed"

// WebFiles contains the embedded web console frontend assets.
//go:embed web/index.html web/style.css web/app.js
var WebFiles embed.FS
