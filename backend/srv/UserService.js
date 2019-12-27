//MOCK service
module.exports = (srv) => {

   /* srv.on('READ', 'Users', () => [
        {
            usid: 'U001', name: "Batman",
            toCars: [
                {crid: "C001", usid: "U001", name: "BatMobile", toUser: {usid: 'U001', name: "Batman"}}
            ],
            toAddress: [
                { adid: "A001", usid: "U001", city: "Gotam", strt: "unknown" }
            ]
        }
    ]);

    srv.on('READ', 'Address', () => [
        { adid: "A001", usid: "U001", city: "Gotam", strt: "unknown" }
    ]);

    srv.on('READ', 'Cars', () => [
        {crid: "C001", usid: "U001", name: "BatMobile", toUser: {usid: 'U001', name: "Batman"}}
    ]);*/


    srv.on('READ', 'Users', () => [
        {

            usid: 'U001', name: "Batman",
            toCars: [
                {crid: "C001", usid: "U001", name: "BatMobile", toUser: {usid: 'U001', name: "Batman"}}
            ],
            toAddress: [
                { adid: "A001", usid: "U001", city: "Gotam", strt: "unknown" }
            ]
        },
	{
            usid: 'U002', name: "Kolya",
            toCars: [
                {crid: "C002", usid: "U002", name: "LadaGranta", toUser: {usid: 'U002', name: "Kolya"}}
            ],
            toAddress: [
                { adid: "A002", usid: "U002", city: "Borovlyany", strt: "Polevaya2" }
            ]
	}
    ]);

    srv.on('READ', 'Address', () => [
        { adid: "A002", usid: "U002", city: "Borovlyany", strt: "Polevaya2" },
        { adid: "A001", usid: "U001", city: "Gotam", strt: "unknown" }
    ]);

    srv.on('READ', 'Cars', () => [
        {crid: "C002", usid: "U002", name: "LadaGranta", toUser: {usid: 'U002', name: "Kolya"}},
        {crid: "C001", usid: "U001", name: "BatMobile", toUser: {usid: 'U001', name: "Batman"}}
    ]);

};
