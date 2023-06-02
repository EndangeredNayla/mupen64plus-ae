package paulscode.android.mupen64plusae-mpn.dialog;

import paulscode.android.mupen64plusae-mpn.MenuListView;
import android.app.Dialog;
import android.app.AlertDialog.Builder;
import android.os.Bundle;

import androidx.annotation.NonNull;
import androidx.fragment.app.DialogFragment;
import android.util.Log;
import android.view.MenuItem;

public class MenuDialogFragment extends DialogFragment
{
    private static final String STATE_DIALOG_ID = "STATE_DIALOG_ID";
    private static final String STATE_TITLE = "STATE_TITLE";
    private static final String STATE_MENU_RESOURCE_ID = "STATE_MENU_RESOURCE_ID";

    public interface OnDialogMenuItemSelectedListener
    {
        /*
         * Called when creating the menu
         */
        void onPrepareMenuList(MenuListView listView);

        /**
         * Called when a dialog menu item is selected
         * 
         * @param dialogId
         *            The parameter ID.
         * @param item
         *            Selected menu item.
         */
        void onDialogMenuItemSelected(int dialogId, MenuItem item);
    }

    public static MenuDialogFragment newInstance(int dialogId, String title, int menuResourceId)
    {
        MenuDialogFragment frag = new MenuDialogFragment();
        Bundle args = new Bundle();
        args.putInt(STATE_DIALOG_ID, dialogId);
        args.putString(STATE_TITLE, title);
        args.putInt(STATE_MENU_RESOURCE_ID, menuResourceId);

        frag.setArguments(args);
        return frag;
    }

    @NonNull
    @Override
    public Dialog onCreateDialog(Bundle savedInstanceState)
    {
        final int dialogId = getArguments() != null ? getArguments().getInt(STATE_DIALOG_ID) : 0;
        String title = getArguments().getString(STATE_TITLE);
        final int menuResourceId = getArguments().getInt(STATE_MENU_RESOURCE_ID);
        MenuListView menuList = new MenuListView(getContext(), null);

        if (getActivity() instanceof OnDialogMenuItemSelectedListener)
        {
            menuList.setMenuResource(menuResourceId);

            ((OnDialogMenuItemSelectedListener) getActivity()).onPrepareMenuList(menuList);
            // Handle menu item selections
            menuList.setOnClickListener((MenuListView.OnClickListener) menuItem -> {
                ((OnDialogMenuItemSelectedListener) getActivity()).onDialogMenuItemSelected(dialogId, menuItem);

                dismiss();
            });

        }
        else
        {
            Log.e("MenuDialogFragment", "Activity doesn't implement OnDialogMenuItemSelected");
        }

        Builder builder = new Builder(getActivity());
        builder.setTitle(title);
        builder.setView(menuList);

        return builder.create();
    }
}