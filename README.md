# devils_engine
Yet Another Gamedev framework / engine

## mingw
нужно поменять флаги компилции для того чтобы он правильно подхватил winsock2.h (вместо sys/select.h) для RE-flex
https://github.com/Genivia/RE-flex/issues/232
но вообще бы от RE-flex избавиться....

## gtl/phmap
с некоторых пор библиотека начала пользоваться std::hardware_*_interference_size, а эта штука опасная
закинул ишшью разрабу https://github.com/greg7mdp/gtl/issues/43
