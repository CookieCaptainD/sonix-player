#!/usr/bin/env python3
"""Port the keys added by the combined PRs into the two newer languages."""
import os

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
LANG = os.path.join(ROOT, "sonix-packer", "assets", "{model}", "usr", "resource", "sonix", "language")

DEAD = ("bt_pairing_request", "bt_enter_pin", "bt_enter_passkey", "bt_confirm_passkey", "bt_confirm_passkey_hint")

# European Portuguese wording where the Brazilian value reads Brazilian.
PT = {
    "volume_limit_confirm_note": "Esta saída vai reproduzir mais alto do que o seu limite de segurança até mudar de saída, alterar o limite ou reiniciar.",
    "collection_delete_title": "Eliminar a coleção?",
    "collection_delete_message": '"%s" será eliminada. Os álbuns permanecem na biblioteca.',
    "collection_cannot_delete": "Não foi possível eliminar a coleção",
    "collection_loading": "A carregar a coleção...",
    "collection_missing_note": "Este álbum já não está na biblioteca.",
    "collection_name_taken": "Já existe uma coleção com este nome",
    "playlist_overwrite_warning": "As alterações feitas nestas playlists no leitor serão perdidas.",
    "power_usb_wake": "Acordar com cabo USB",
    "settings_skip_buttons": "Botões de avanço",
    "settings_skip_seek": "Pesquisar",
    "settings_skip_note": "Mudar de faixa: um toque curto muda de faixa ou episódio; segurar pesquisa.\\nPesquisar: um toque curto avança dentro do item; segurar repete. No R1, segurar Play rebobina no modo Pesquisar. A música usa o passo abaixo; livros e podcasts mantêm os seus.",
    "settings_seek_step": "Passo de pesquisa",
    "coverfetch_find_wifi_note": "O Wi-Fi está desligado. Ligue-o para procurar uma capa no MusicBrainz: a pesquisa começa assim que estiver ligado.",
    "coverfetch_find_searching": "A procurar uma capa...",
    "coverfetch_find_no_tags": "A faixa precisa de álbum e artista nas etiquetas.",
    "coverfetch_find_failed": "Não foi possível contactar o MusicBrainz. Tente novamente mais tarde.",
    "coverfetch_find_saved": "Capa guardada",
    "coverfetch_batch_title": "Descarregar capas",
    "coverfetch_batch_note": "Encontre e guarde capas de álbuns que não têm.",
    "coverfetch_batch_download": "Descarregar",
    "coverfetch_batch_cancelled": "Transferência interrompida",
    "coverfetch_batch_wifi_title": "Descarregar capas?",
    "coverfetch_batch_wifi_note": "O Wi-Fi está desligado. Ligue-o para procurar capas no MusicBrainz.",
}


def read(path):
    rows = []
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            if "=" in line and not line.startswith(";"):
                k, v = line.split("=", 1)
                rows.append((k.strip(), v.strip()))
    return rows


def write_rows(path, rows):
    with open(path, "w", encoding="utf-8") as f:
        for k, v in rows:
            f.write(f"{k} = {v}\n")


for model in ("R1", "R3PII"):
    folder = LANG.format(model=model)
    en = dict(read(os.path.join(folder, "English.ini")))
    br = dict(read(os.path.join(folder, "Português BR.ini")))

    # The five PIN strings came from local car-mode work and have no call site.
    for name in ("English.ini", "Português BR.ini"):
        path = os.path.join(folder, name)
        rows = [(k, v) for k, v in read(path) if k not in DEAD]
        write_rows(path, rows)

    for name in ("Português PT.ini", "Ukrainian.ini"):
        path = os.path.join(folder, name)
        rows = read(path)
        have = {k for k, _ in rows}
        missing = [k for k in en if k not in have and k not in DEAD]
        added = []
        for k in missing:
            value = PT.get(k, br.get(k, en[k])) if name.startswith("Portugu") else en[k]
            added.append((k, value))
        with open(path, "a", encoding="utf-8") as f:
            f.write("\n; --- strings added by the combined PR build\n")
            for k, v in added:
                f.write(f"{k} = {v}\n")
        print(f"{model}/{name}: +{len(added)} keys")
