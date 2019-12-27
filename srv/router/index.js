"use strict";

module.exports = (app, server) => {
    app.use("/user", require("./routes/user")());
};
