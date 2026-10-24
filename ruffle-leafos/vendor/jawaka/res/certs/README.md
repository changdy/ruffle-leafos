# CA certificate bundle

`cacert.pem` is the Mozilla CA root bundle as published by the curl project at
<https://curl.se/ca/cacert.pem>. It is used by the Jawaka OTA updater
(`internal/update/update.c`) as `CURLOPT_CAINFO` so HTTPS verification does not
depend on the stock firmware's CA store (some LoongOS builds ship a libcurl whose
compiled-in default CA path does not exist on the device).

It is staged into the launcher bundle at `res/certs/cacert.pem`.

To refresh:

```sh
curl -fsSL -o res/certs/cacert.pem https://curl.se/ca/cacert.pem
```

License: the bundle carries Mozilla's MPL 2.0 certificate data; see the header
of `cacert.pem`.
