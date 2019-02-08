## Building

```
git clone https://git.jordanko.ch/eskimo/Igloo-Push.git
cd Push
make
```

If you want to use libcurl replace `make` with `make curl=yes`

## Usage

`make install`
 
Then on IRC

`/msg *status loadmod --type=user push`

## Configuration

`/msg *push set device1 token`

* Replace device1 with device2-5 for additional devices
* Replace token with your push token from Settings in Igloo

## Testing

`/msg *push send test`