#pragma once

#include <QColor>
#include <QObject>
#include <QString>

// Paleta del diseño "Editor de Video.dc.html", expuesta a QML como el singleton
// `Theme`. Es un singleton de C++ (no un archivo QML con `pragma Singleton`)
// porque los singletons basados en archivo QML se crean de forma perezosa en el
// primer acceso dentro de un binding; durante la construcción síncrona del árbol
// eso hacía que cientos de bindings vieran `Theme.*` como `undefined` en su
// primera evaluación ("Unable to assign [undefined] to QColor"). Registrado con
// qmlRegisterSingletonInstance en main.cpp, la instancia existe antes de cargar
// la UI, igual que TimelineModel/Compositor/etc., así que nunca queda indefinido.
class Theme : public QObject
{
    Q_OBJECT

    // Fondos
    Q_PROPERTY(QColor bg0 MEMBER m_bg0 CONSTANT)
    Q_PROPERTY(QColor bg1 MEMBER m_bg1 CONSTANT)
    Q_PROPERTY(QColor bg2 MEMBER m_bg2 CONSTANT)
    Q_PROPERTY(QColor bg3 MEMBER m_bg3 CONSTANT)
    Q_PROPERTY(QColor panel MEMBER m_panel CONSTANT)
    Q_PROPERTY(QColor panel2 MEMBER m_panel2 CONSTANT)
    Q_PROPERTY(QColor panel3 MEMBER m_panel3 CONSTANT)
    Q_PROPERTY(QColor panelHead MEMBER m_panelHead CONSTANT)
    Q_PROPERTY(QColor sunken MEMBER m_sunken CONSTANT)
    Q_PROPERTY(QColor sunken2 MEMBER m_sunken2 CONSTANT)

    // Líneas
    Q_PROPERTY(QColor line MEMBER m_line CONSTANT)
    Q_PROPERTY(QColor line2 MEMBER m_line2 CONSTANT)
    Q_PROPERTY(QColor line3 MEMBER m_line3 CONSTANT)
    Q_PROPERTY(QColor lineSoft MEMBER m_lineSoft CONSTANT)
    Q_PROPERTY(QColor hover MEMBER m_hover CONSTANT)
    Q_PROPERTY(QColor hover2 MEMBER m_hover2 CONSTANT)

    // Texto
    Q_PROPERTY(QColor text MEMBER m_text CONSTANT)
    Q_PROPERTY(QColor textHi MEMBER m_textHi CONSTANT)
    Q_PROPERTY(QColor textMid MEMBER m_textMid CONSTANT)
    Q_PROPERTY(QColor textDim MEMBER m_textDim CONSTANT)
    Q_PROPERTY(QColor textFaint MEMBER m_textFaint CONSTANT)

    // Acentos
    Q_PROPERTY(QColor amber MEMBER m_amber CONSTANT)
    Q_PROPERTY(QColor amberInk MEMBER m_amberInk CONSTANT)
    Q_PROPERTY(QColor blue MEMBER m_blue CONSTANT)
    Q_PROPERTY(QColor green MEMBER m_green CONSTANT)
    Q_PROPERTY(QColor purple MEMBER m_purple CONSTANT)
    Q_PROPERTY(QColor red MEMBER m_red CONSTANT)

    // Tipografías
    Q_PROPERTY(QString mono MEMBER m_mono CONSTANT)
    Q_PROPERTY(QString sans MEMBER m_sans CONSTANT)

public:
    explicit Theme(QObject *parent = nullptr) : QObject(parent) {}

private:
    QColor m_bg0{"#0c0d10"};
    QColor m_bg1{"#131418"};
    QColor m_bg2{"#16171b"};
    QColor m_bg3{"#17181c"};
    QColor m_panel{"#1c1d22"};
    QColor m_panel2{"#1b1c21"};
    QColor m_panel3{"#191a1f"};
    QColor m_panelHead{"#212228"};
    QColor m_sunken{"#141519"};
    QColor m_sunken2{"#0a0b0d"};

    QColor m_line{"#2b2c33"};
    QColor m_line2{"#33353d"};
    QColor m_line3{"#050507"};
    QColor m_lineSoft{"#26272d"};
    QColor m_hover{"#2b2d34"};
    QColor m_hover2{"#26282f"};

    QColor m_text{"#cdd0d7"};
    QColor m_textHi{"#e6e8ec"};
    QColor m_textMid{"#a4a7af"};
    QColor m_textDim{"#7f838c"};
    QColor m_textFaint{"#6b6e77"};

    QColor m_amber{"#e2a24b"};
    QColor m_amberInk{"#1a1206"};
    QColor m_blue{"#5b8dd6"};
    QColor m_green{"#4a9e6b"};
    QColor m_purple{"#8a6bc0"};
    QColor m_red{"#c0392b"};

    QString m_mono{"Cascadia Code, Consolas, monospace"};
    QString m_sans{"Segoe UI"};
};
