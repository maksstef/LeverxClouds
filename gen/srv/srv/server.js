"use strict";

const https = require("https");
const port = process.env.PORT || 3000;
const server = require("http").createServer();
const xsenv = require("@sap/xsenv");
const xssec = require("@sap/xssec");
const xsHDBConn = require("@sap/hdbext");
const express = require("express");
const logging = require("@sap/logging");
const compression = require("compression");
const cds = require("@sap/cds");
const bodyParser = require('body-parser');

https.globalAgent.options.ca = xsenv.loadCertificates();
global.__base = __dirname + "/";

// Initialize Express App
const app = express();

//Body parser
app.use(bodyParser.json());

//Compression
app.use(compression({
    threshold: "1b"
}));

const hanaOptions = xsenv.getServices({
     hana: {
         plan: "hdi-shared"
     }
});

hanaOptions.hana.pooling = true;
app.use(
    xsHDBConn.middleware(hanaOptions.hana)
);

/*
//CDS OData V4 Handler
cds.connect({
    kind: "hana",
    logLevel: "info",
    credentials: hanaOptions.hana
});
cds
    .serve("gen/csn.json", {
        crashOnError: false
    })
    .at("/odata/")
    .in(app)
    .catch(err => {
        // do not crash on error
    });
*/

//Setup Routes
require("./router")(app, server);

//Error handling
app.use(function (err, req, res, next) {
        res.status(500).send({
            "msgId": "",
            "type": "Error",
            "msg": err.message
        });
});

//Start the Server
server.on("request", app);
server.listen(port, () => console.info(`HTTP Server: ${server.address().port}`));
