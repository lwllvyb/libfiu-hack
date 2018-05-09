## build

```shell
make
make install
make python2_install
make python3_install
```

## run

### run the program with tcp server
```shell
FIU_ENABLE_PORT=20000 fiu-run -x top
```

This command will create a tcp server thread with ip 0.0.0.0 port 20000.(You can use FIU_ENABLE_IP=x.x.x.x to assign the ip addr)

### send inject fault command to program

```shell
fiu-tcp 127.0.0.1 20000 "enable name=posix/io/rw/*"
```


